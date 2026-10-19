#!/usr/bin/env python3
"""
Seals streaming-keys.ini into streaming-keys.bin, the form the firmware ships
it in.

    streaming-keys.ini      the keys in the clear -- never in git, never in an image
    streaming-keys.key      the key that seals them -- never in git
        |
        v
    streaming-keys.bin      one per assets tree, in usr/resource/morpthis/components
    build_gen/streamkeys_key.h      the same key, written by the Makefile
                                    and compiled into the player

    python3 tools/seal_streamkeys.py seal [streaming-keys.ini] [-o OUT ...]
    python3 tools/seal_streamkeys.py open streaming-keys.bin

`seal` makes streaming-keys.key the first time and reuses it afterwards; with
no -o it writes into every assets tree of sonix-packer. `open` prints what a
.bin holds, to check it. Rebuild the player after a new key.

What this buys: the image no longer carries the keys in a file anyone can read
after unpacking it. What it does not: the key is in the binary, and whoever
takes the binary apart gets the keys back, as was done with HiBy's own.

The format, all of it hashed with the sha1() the player already has:

    "SXK1" | nonce (16) | tag (20) | ciphertext

    block i of keystream = SHA1(key | nonce | i as 32-bit big-endian)
    tag                  = SHA1(key | nonce | plaintext)

The tag is there to tell a .bin sealed with another key from a good one, not
to stand up to anyone.
"""

import argparse
import hashlib
import os
import sys

PLAYER_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKER_ASSETS = os.path.join(os.path.dirname(PLAYER_ROOT), "sonix-packer", "assets")
COMPONENTS = os.path.join("usr", "resource", "morpthis", "components")

DEFAULT_INI = os.path.join(PLAYER_ROOT, "streaming-keys.ini")
DEFAULT_KEY = os.path.join(PLAYER_ROOT, "streaming-keys.key")

MAGIC = b"SXK1"
KEY_LEN = 32
NONCE_LEN = 16
TAG_LEN = 20
# Mirrors STREAMKEYS_MAX_FILE in streamkeys.c, which counts the whole .bin.
MAX_FILE = 16384


def die(msg):
    print(f"seal_streamkeys: {msg}", file=sys.stderr)
    sys.exit(1)


def read_key(path):
    try:
        with open(path) as f:
            text = f.read().strip()
    except FileNotFoundError:
        return None
    try:
        key = bytes.fromhex(text)
    except ValueError:
        die(f"{path} is not hex")
    if len(key) != KEY_LEN:
        die(f"{path} holds {len(key)} bytes, not {KEY_LEN}")
    return key


def keystream(key, nonce, length):
    out = bytearray()
    counter = 0
    while len(out) < length:
        out += hashlib.sha1(key + nonce + counter.to_bytes(4, "big")).digest()
        counter += 1
    return bytes(out[:length])


def seal(key, plain):
    nonce = os.urandom(NONCE_LEN)
    tag = hashlib.sha1(key + nonce + plain).digest()
    cipher = bytes(a ^ b for a, b in zip(plain, keystream(key, nonce, len(plain))))
    return MAGIC + nonce + tag + cipher


def unseal(key, blob):
    if blob[: len(MAGIC)] != MAGIC:
        die("not a sealed keys file")
    head = len(MAGIC) + NONCE_LEN + TAG_LEN
    nonce = blob[len(MAGIC) : len(MAGIC) + NONCE_LEN]
    tag = blob[len(MAGIC) + NONCE_LEN : head]
    cipher = blob[head:]
    plain = bytes(a ^ b for a, b in zip(cipher, keystream(key, nonce, len(cipher))))
    if hashlib.sha1(key + nonce + plain).digest() != tag:
        die("sealed with another key")
    return plain


def asset_targets():
    if not os.path.isdir(PACKER_ASSETS):
        return []
    out = []
    for model in sorted(os.listdir(PACKER_ASSETS)):
        comp = os.path.join(PACKER_ASSETS, model, COMPONENTS)
        if os.path.isdir(comp):
            out.append(os.path.join(comp, "streaming-keys.bin"))
    return out


def cmd_seal(args):
    try:
        with open(args.ini, "rb") as f:
            plain = f.read()
    except FileNotFoundError:
        die(f"no {args.ini}")
    head = len(MAGIC) + NONCE_LEN + TAG_LEN
    if head + len(plain) > MAX_FILE:
        die(f"{args.ini} is over {MAX_FILE - head} bytes")

    key = read_key(args.key)
    if key is None:
        key = os.urandom(KEY_LEN)
        with open(args.key, "w") as f:
            f.write(key.hex() + "\n")
        print(f"new key in {args.key} -- keep it, and rebuild the player")

    targets = args.out or asset_targets()
    if not targets:
        die("no sonix-packer assets found; say where with -o")
    for path in targets:
        with open(path, "wb") as f:
            f.write(seal(key, plain))
        print(f"sealed {path}")
        plain_ini = os.path.join(os.path.dirname(path), "streaming-keys.ini")
        if os.path.exists(plain_ini):
            print(f"  {plain_ini} is still there in the clear: remove it")


def cmd_open(args):
    key = read_key(args.key)
    if key is None:
        die(f"no {args.key}")
    with open(args.bin, "rb") as f:
        sys.stdout.write(unseal(key, f.read()).decode("utf-8", "replace"))


def main():
    p = argparse.ArgumentParser(description="Seal streaming-keys.ini for the firmware.")
    p.add_argument("--key", default=DEFAULT_KEY, help="the sealing key (default: %(default)s)")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("seal", help="write streaming-keys.bin")
    s.add_argument("ini", nargs="?", default=DEFAULT_INI)
    s.add_argument("-o", "--out", action="append", help="where to write it (repeatable)")
    s.set_defaults(func=cmd_seal)

    o = sub.add_parser("open", help="print what a streaming-keys.bin holds")
    o.add_argument("bin")
    o.set_defaults(func=cmd_open)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
