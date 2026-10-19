#!/usr/bin/env python3
"""Real localhost HTTP integration; no external server/account/network needed."""
import hashlib
import json
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

import tls_fixtures

USER = "alice+user@example.com"
PASSWORD = 'p &\\"ä'


def pattern(length, seed):
    unit = bytes((i + seed) % 256 for i in range(256))
    return (unit * ((length + 255) // 256))[:length]


def song(identifier, title="Música ♪", suffix="flac", number=1):
    return {"id": identifier, "title": title, "artist": "Artist", "album": "Album",
            "suffix": suffix, "duration": 180, "track": number, "isDir": False}


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, cert=None, key=None):
        super().__init__(("0.0.0.0", 0), Handler)
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.errors = []
        self.requests = []
        self.paths = []
        self.media = {}
        if cert:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            self.socket = context.wrap_socket(self.socket, server_side=True)

    # A client that refuses the certificate aborts the handshake, which is the
    # test working as intended rather than a server fault; socketserver would
    # otherwise print a traceback for each refusal.
    def handle_error(self, request, client_address):
        pass


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *_):
        pass  # Never print authenticated URLs, even in a test server.

    def body(self, content, mime="application/octet-stream", status=200, length=None):
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", len(content) if length is None else length)
        self.end_headers()
        try:
            for start in range(0, len(content), 32768):
                self.wfile.write(content[start:start + 32768])
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def document(self, response, status=200):
        self.body(json.dumps({"subsonic-response": {"status": "ok", "version": "1.16.1", **response}},
                             ensure_ascii=True).encode(), "application/json", status)

    def do_GET(self):
        try:
            self.handle_get()
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when a stale producer or decoder lease is canceled.
        except Exception as exc:
            with self.server.lock:
                self.server.errors.append(repr(exc))
            self.body(b"test server assertion failed", status=500)

    def handle_get(self):
        request = urlsplit(self.path)
        with self.server.lock:
            self.server.paths.append(request.path)
        q = {key: values[0] for key, values in parse_qs(request.query).items()}
        path = request.path
        if path.startswith("/rest/"):
            assert q.get("u") == USER, q
            assert q.get("f") == "json" and q.get("v") == "1.16.1" and q.get("c") == "morpthis", q
            assert "p" not in q and len(q.get("s", "")) == 32, q
            token = hashlib.md5((PASSWORD + q["s"]).encode()).hexdigest()
            if q.get("t") != token:
                self.document({"status": "failed", "error": {"code": 40, "message": "Invalid credentials"}}, 401)
                return
            with self.server.lock:
                self.server.requests.append((path, q))
            endpoint = path.rsplit("/", 1)[1]
            if endpoint == "ping.view":
                self.document({})
            elif endpoint == "getMusicFolders.view":
                self.document({"musicFolders": {"musicFolder": [
                    {"id": 7, "name": "Library"},
                    {"id": 123456789012345678901234567890, "name": "Large ID"},
                    {"id": 12.5, "name": "Invalid fraction"},
                    {"id": -1, "name": "Invalid negative number"},
                    {"id": "x" * 128, "name": "Overlong identity"},
                    {"id": "before\u0000after", "name": "Embedded NUL"}]}})
            elif endpoint == "getIndexes.view":
                assert q["musicFolderId"] == "7"
                self.document({"indexes": {"index": [{"name": "A", "artist": [{"id": "folder/A&B", "name": "Artist"}]}],
                                             "child": [song("loose")]}})
            elif endpoint == "getMusicDirectory.view":
                assert q["id"] == "folder/A&B"
                self.document({"directory": {"id": q["id"], "child": [
                    {"id": "folder/album", "title": "Album folder", "isDir": True},
                    song("song/1?+"), song("song/2= &", number=2)]}})
            elif endpoint == "getArtists.view":
                artists = [{"id": f"artist/{i}", "name": f"Àrtist {i}", "albumCount": 1} for i in range(205)]
                self.document({"artists": {"index": [{"name": "A", "artist": artists}]}})
            elif endpoint == "getArtist.view":
                assert q["id"] == "artist/0"
                self.document({"artist": {"id": q["id"], "album": [{"id": "album/0", "name": "Album", "artist": "Artist"}]}})
            elif endpoint == "getAlbum.view":
                if q["id"] == "auth-error":
                    self.document({"status": "failed", "error": {"code": 41, "message": "Token auth required"}})
                elif q["id"] == "auth-text":
                    self.body(b"Forbidden", "text/plain", 403)
                elif q["id"] == "bad-json":
                    self.body(b"not JSON", "application/json")
                elif q["id"] == "too-large":
                    self.body(b"x" * (2 * 1024 * 1024 + 1), "application/json")
                elif q["id"] == "too-many-tokens":
                    self.body(b"[" + b"0," * 65536 + b"0]", "application/json")
                else:
                    assert q["id"] == "album/0"
                    self.document({"album": {"id": q["id"], "song": [song("song/1?+"), song("song/2= &", number=2)]}})
            elif endpoint == "stream.view":
                assert q.get("format") in ("mp3", "opus", "raw")
                if q["format"] == "raw":
                    assert "maxBitRate" not in q
                else:
                    assert q.get("maxBitRate") in ("256", "320")
                if q["id"] == "stream-fail":
                    self.document({"status": "failed", "error": {"code": 40, "message": "Stream refused"}})
                elif q["id"] in self.server.media:
                    content, mime = self.server.media[q["id"]]
                    self.body(content, mime)
                else:
                    assert q["id"] in ("blob-a", "blob-b"), q
                    self.body(pattern(3 * 1024 * 1024, 17 if q["id"] == "blob-a" else 91))
            else:
                raise AssertionError(endpoint)
            return
        if path == "/pattern":
            self.body(pattern(int(q["length"]), int(q["seed"])))
        elif path == "/header-race":
            self.send_response(200)
            self.send_header("Content-Length", str(2 * 1024 * 1024))
            self.end_headers()
            self.wfile.write(pattern(512 * 1024, 8))
            self.wfile.flush()
            self.server.stop.wait(0.1)
            self.wfile.write(pattern(1536 * 1024, 8))
        elif path == "/chunked":
            content = pattern(int(q["length"]), int(q["seed"]))
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(content), 1997):
                chunk = content[i:i + 1997]
                self.wfile.write(f"{len(chunk):x};test=yes\r\n".encode() + chunk + b"\r\n")
            self.wfile.write(b"0\r\nX-Test: trailer\r\n\r\n")
        elif path == "/radio-chunked":
            audio = pattern(128, 55)
            title = b"StreamTitle='Test radio';"
            metadata = bytes([2]) + title + bytes(32 - len(title))
            content = b"".join(audio[i:i + 8] + metadata for i in range(0, len(audio), 8))
            self.send_response(200)
            self.send_header("icy-metaint", "8")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(content), 5):
                chunk = content[i:i + 5]
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif path == "/chunked-malformed":
            malformed = {"sign": b"-1\r\nx\r\n0\r\n\r\n", "space": b" 1\r\nx\r\n0\r\n\r\n",
                         "overflow": b"10000000000000000\r\n", "long-line": b"1;" + b"x" * 256 + b"\r\nx",
                         "no-zero": b"1\r\nx\r\n", "bad-crlf": b"1\r\nxJUNK\r\n0\r\n\r\n",
                         "no-trailer-end": b"1\r\nx\r\n0\r\n", "nul": b"1\x00ignored\r\nx\r\n0\r\n\r\n",
                         "long-trailers": b"0\r\n" + (b"X-Test: " + b"x" * 240 + b"\r\n") * 40 + b"\r\n"}
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(malformed[q["variant"]])
        elif path == "/chunked-retry":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"1")
            self.wfile.flush()
            self.server.stop.wait(1.3)  # Timeout inside a hexadecimal size.
            self.wfile.write(b"00;extension=yes\r\n" + pattern(256, 77) + b"\r")
            self.wfile.flush()
            self.server.stop.wait(1.3)  # Timeout between CR and LF.
            self.wfile.write(b"\n0\r\nX-Final: trailer\r")
            self.wfile.flush()
            self.server.stop.wait(1.3)  # Timeout inside a final trailer.
            self.wfile.write(b"\n\r\n")
        elif path == "/chunked-broken":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"100\r\nshort")  # EOF inside a chunk
        elif path == "/truncated":
            self.body(b"cut short", length=128 * 1024)
        elif path == "/huge-length":
            self.body(b"cut short", length="4294967403 \t")
        elif path == "/empty":
            self.body(b"")
        elif path == "/stream-error":
            self.document({"status": "failed", "error": {"code": 40, "message": "Not authorized"}})
        elif path == "/headers-stall":
            self.server.stop.wait(20)
        elif path == "/body-stall":
            self.send_response(200)
            self.send_header("Content-Length", 2 * 1024 * 1024)
            self.end_headers()
            self.wfile.write(bytes(8192))
            self.wfile.flush()
            self.server.stop.wait(20)
        else:
            self.body(b"not found", status=404)


def start_https_servers(directory):
    """One HTTPS server per certificate: a good self-signed one, one naming a
    different server, and one that has expired."""
    certificates = {
        "good": (tls_fixtures.GOOD_CERT, tls_fixtures.GOOD_KEY),
        "wrongname": (tls_fixtures.WRONG_NAME_CERT, tls_fixtures.WRONG_NAME_KEY),
        "expired": (tls_fixtures.EXPIRED_CERT, tls_fixtures.EXPIRED_KEY),
    }
    started = []
    for name, (certificate, key) in certificates.items():
        cert_path = pathlib.Path(directory) / f"{name}.pem"
        key_path = pathlib.Path(directory) / f"{name}.key"
        cert_path.write_text(certificate, encoding="utf-8")
        key_path.write_text(key, encoding="utf-8")
        server = Server(str(cert_path), str(key_path))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        started.append(server)
    return started


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    server = Server()
    if binary.name == "test_codecs":
        from media_fixtures import make_media
        server.media = make_media()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="morpthis-subsonic-test-") as directory:
            tls = start_https_servers(directory)
            try:
                bases = [f"https://127.0.0.1:{s.server_port}" for s in tls]
                result = subprocess.run([str(binary), f"http://127.0.0.1:{server.server_port}", directory, *bases],
                                        text=True, capture_output=True, timeout=45)
            finally:
                for s in tls:
                    s.stop.set()
                    s.shutdown()
                    s.server_close()
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        assert not server.errors, server.errors
        for s in tls:
            assert not s.errors, s.errors
        assert PASSWORD not in result.stderr and "&t=" not in result.stderr and "&s=" not in result.stderr
        if result.returncode:
            raise SystemExit(result.returncode if result.returncode > 0 else 1)
        assert server.requests, "test made no actual HTTP requests"
        failed = [q for _, q in server.requests if q.get("id") == "stream-fail"]
        if binary.name == "test_subsonic":
            assert len(failed) == 1, "failed pre-cache was repeatedly re-fetched"
            # The relaxed-mode connection has to have reached the server: one
            # request for the certificate test, one for the stream that outlives
            # the switch back to strict. The two refusals never get this far.
            assert tls[0].paths == ["/pattern", "/pattern"], tls[0].paths
        print(f"PASS mock server verified {len(server.requests)} authenticated requests; no credentials in logs")
    finally:
        server.stop.set()
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
