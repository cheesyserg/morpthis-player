# Subsonic / Gonic on the TempoTec V1

## Setup

1. Connect the V1 to Wi-Fi.
2. Open **Streaming → Subsonic → Server settings**. This destination is in both the regular grid and the compact Tokyo Night list.
3. Enter the server base URL, username and password. For example, `https://music.example.com` or `http://192.168.1.20:4747`. A reverse-proxy prefix such as `/gonic` is supported; a pasted trailing `/rest/` is normalized. Use an IPv4 address or DNS name, not a literal IPv6 URL.
4. Choose **Connect / save**. Saving an account stops existing Subsonic playback and invalidates its old browser/queue IDs; select a track again after reconnecting. A worker checks `ping.view`; the UI never waits in a socket call. No Qobuz/Tidal/Podcast application key is required.
5. Browse **Music folders** or **Artists**, then select a track. Tracks from the **current page** become the queue (up to 200); use Previous/Next page for larger directories or albums.

### Quality

| Setting | Stream request | Use |
| --- | --- | --- |
| MP3, 256 (default) | `format=mp3&maxBitRate=256` | Conservative CPU/bandwidth default |
| MP3, 320 | `format=mp3&maxBitRate=320` | Higher bitrate |
| Opus, 256/320 | `format=opus&maxBitRate=256` or `320` | Server-provided Opus; output is 48 kHz |
| Raw | `format=raw`, no `maxBitRate` | Original **FLAC, MP3 or Opus** on a good home network |

The bitrate is a **server-side ceiling**, not a promise that every file will be encoded at that rate. Configure/enable transcoding on Gonic or your compatible server. If the server ignores the requested format, playback refuses the mismatched codec rather than scanning the stream for another decoder.

Raw means the original compressed file, **not PCM**. Recognized suffixes are `flac`, `mp3` and `opus`; Opus stored as `.ogg` should be transcoded too. WAV, MP4/AAC/ALAC, Vorbis, DSD, APE and other originals are not handled by this forward-readable bridge; choose MP3/Opus transcoding instead. Unsupported raw entries are omitted from a page's playback queue, with an error if the selected track itself is unsupported. A quality change applies to the next queue created by selecting a track, not to a queue already playing.

### Credentials and TLS

- The password is shown as plain text while editing so you can verify what you entered. Keep the screen private while editing.
- Authentication is Subsonic v1.16.1 token authentication: random 16-byte salt, `MD5(password + salt)`, `u/t/s`, `c=sonix`, `f=json`. The password is never put in a URL.
- Credentials stay in memory unless **Remember credentials** is enabled. The optional `subsonic-credentials.json` beside `device_config.ini` is written atomically with mode **0600**. It contains the password **unencrypted**; do not share it or backups of it. Storage must support the requested permissions.
- Clearing Remember and saving removes the old saved credentials. **Forget server?** stops this service and removes its account from memory and disk.
- Prefer HTTPS with a trusted certificate and a correct player clock. A salted MD5 token can still be replayed and does not make HTTP private. HTTPS-to-HTTP redirects are refused; there is no silent TLS downgrade.
- A server with a self-signed certificate is refused by default. **Accept self-signed certificates** on this settings page (or `[network] tls_verify = relaxed` in `/usr/data/device_config.ini`) accepts a certificate from an authority no bundle knows, and only that: the certificate must still name the server being contacted and its dates must still cover the current date. An expired certificate, one issued to another name, and an HTTPS-to-HTTP redirect stay refusals, with a message saying which. The setting applies to every `https://` connection the player opens, radio included; the switch takes effect on the next connection, without a restart. See [your own server with a self-signed certificate](../README.md#your-own-server-with-a-self-signed-certificate).
- For a private certificate authority, use the existing `[network] ca_bundle` setting/trust store instead: it keeps full verification on. See the [TLS troubleshooting section](../README.md#lastfm-sign-in-tls-certificate-error); it applies to Subsonic too. Do not replace the trust store with an untrusted certificate bundle.
- Playback queue names are credential-free, ephemeral `subsonic://...` source IDs, not HTTP URLs. They and their positions are not restored after reboot. Generic HTTP diagnostic URLs have their query removed.

## Transport and playback

```text
GUI browse worker ── bounded JSON REST calls ── metadata/page mailbox

HTTP/TLS producer A ── 1 MiB RAM ring ── local codec callbacks ── existing DSP/ALSA
HTTP/TLS producer B ── 512 KiB next-track pre-cache ── promote to active at handoff
                       (second fixed 1 MiB allocation)
```

- The two rings are allocated lazily; audio caching occupies **2 MiB maximum**, independently of track length. REST response/token arrays, queue metadata, codec memory and worker stacks are additional memory, not part of that figure. No audio is spooled to disk and there is no localhost HTTP proxy.
- Each producer owns all its socket/TLS operations. The decoder only copies RAM and, when necessary, waits on a condition variable. Power-of-two wrap masks avoid 64-bit division on the MIPS read path. There is no per-read allocation or network polling on the audio thread.
- Each new producer/API worker reserves a 256 KiB stack rather than inheriting a multi-MiB default; codec and crash-signal stacks are separate.
- Refill begins at approximately **50%** of the active quota and fills to the high watermark. Otherwise workers sleep. They run at nice 10, not `SCHED_IDLE`, which could starve a needed refill behind UI work.
- Startup primes **512 KiB** or a clean, nonempty short file. At a decode-block boundary, below **64 KiB**, playback waits toward 512 KiB rather than repeatedly decoding tiny arrivals. Stop/replacement can abort waits. Pause interrupts only the block boundary, never an in-progress codec read as fake EOF.
- At approximately **90% playback progress**, the GUI asks the playlist for a nonmutating next-track prediction and fetches its first **512 KiB** in the spare slot. Prefetch starts only when the active reserve is at least 512 KiB or its transfer has completed; an already-started transfer can share bandwidth. Repeat-one uses a fresh instance starting at byte zero; an unknown new shuffle deal cannot be predicted.
- A decoder lease prevents a slot from being reset while a codec still uses it. Stop, local playback and service switches cancel producers without joining them on the UI thread. Generations discard stale data/results.
- Chunked HTTP framing and trailers are removed before audio/ICY data. Framing state survives receive timeouts. Bounded same-connection retries cover transient receive stalls; starvation eventually fails rather than spinning at real-time priority or advancing through a broken queue. Empty bodies, API error pages and truncated Content-Length/chunked transfers fail.
- The initial read/rewind probe is protected from a racing producer. MP3 gets only retained-header rewind/forward skip callbacks; **unknown lengths are never determined by scanning a live stream**. Known Xing/LAME delay/padding is honored. FLAC uses forward/header callbacks; Opus is nonseekable and honors preskip/end trimming. Missing codec lengths use the server duration for progress.
- Playback seeking and local-file waveforms/tag reads are disabled for these RAM sources. Wi-Fi idle/suspend policy recognizes service playback and outstanding HTTP/REST demand.

## Gapless and power: what is and is not guaranteed

The existing ALSA handoff is reused when rate, channels, bit depth and output route match. A ready pre-cache removes the next HTTP request from that critical path. It does **not** pre-initialize a second decoder or resample unrelated formats to force a match.

For a gapless check, enable the player's gapless mode, disable track fades, and use adjacent tracks with matching output parameters. Lossy files/transcodes must carry valid encoder-delay/end-padding information (MP3 Xing/LAME or Opus timing); a live MP3 encoder that omits it can introduce its own padding. FLAC is a useful bit-exact baseline.

A buffer cannot mask an arbitrarily long outage or a connection slower than the audio consumption rate. Short tracks, missing/incorrect server durations, a fresh shuffle round, huge codec headers, route changes and a prefetch that did not finish can still require a startup/rebuffer wait. System DNS resolution is the existing blocking resolver call and is not directly canceled by shutting down a socket.

Worker sleeping, bounded RAM and server transcoding reduce avoidable work, but **no battery-life, CPU-percentage or absolute no-stutter guarantee is claimed**. Those require measurement on the actual V1 and network.

## Validation

See [the test instructions](../sonix-player/tests/README.md) for the real local-HTTP integration harness. It exercises authentication/credentials, browsing/pagination, memory bounds/refill, chunk framing/retry, truncation, cancellation and two-slot ownership. The optional codec suite uses the actual player decoders and deterministic MP3/FLAC/Opus streams, including large headers and exact Opus trimming. Both suites have ASan/UBSan targets.

These are host tests, not hardware certification. Before shipping a firmware build, test on a V1:

- sustained MP3 256/320, Opus, and raw FLAC playback while browsing/redrawing covers;
- transitions between matching tracks with a known continuous recording, including repeat-one/all and shuffle;
- tracks shorter than ten seconds and albums/directories longer than 200 entries;
- pause/resume, rapid Next/Previous/Stop, switching to local/Tidal/Qobuz/podcast playback, and replacing a track during initial buffering;
- temporary Wi-Fi stalls, server disconnects, malformed responses and a server ignoring the selected codec;
- valid/expired/private-CA HTTPS, rejecting a downgrade redirect, and browsing-away during a pending connection;
- a self-signed HTTPS server with the certificate switch off (refused), on (plays), and with a certificate that names another server or has expired (refused either way);
- screen-off playback, automatic Wi-Fi/suspend policy, RAM and CPU use, ALSA underrun logs, and battery drain against a local-playback baseline.

The repository's regular target build/packer discovers the new C sources automatically. The V1 tree carries its own language assets, in `sonix-packer/assets/V1/usr/resource/sonix/language`.
