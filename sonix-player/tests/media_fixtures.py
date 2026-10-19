"""Tiny deterministic, self-generated fixtures: no ffmpeg or downloaded music.

The MP3 frames contain silent MPEG-1 Layer III granules without a reservoir;
FLAC uses lossless verbatim subframes; Opus uses its standard silence packet.
"""
import struct


def crc(data, bits, polynomial):
    value = 0
    mask = (1 << bits) - 1
    for byte in data:
        value ^= byte << (bits - 8)
        for _ in range(8):
            value = ((value << 1) ^ (polynomial if value & (1 << (bits - 1)) else 0)) & mask
    return value


def mp3(padding=0):
    # 128 kbps, 44.1 kHz, stereo, 1152 samples/frame; 2000 frames.
    audio = (b"\xff\xfb\x90\x00" + bytes(413)) * 2000
    if not padding:
        return audio
    synchsafe = bytes((padding >> shift) & 0x7f for shift in (21, 14, 7, 0))
    return b"ID3\x04\x00\x00" + synchsafe + bytes(padding) + audio


def mp3_xing():
    # One metadata frame followed by 2000 audio frames. LAME delay/padding
    # fields are 12-bit sample counts (576 and 1080 here).
    frame = bytearray(b"\xff\xfb\x90\x00" + bytes(413))
    tag = b"Info" + struct.pack(">II", 1, 2000) + b"LAME3.100" + bytes(12)
    tag += bytes([0x24, 0x04, 0x38]) + bytes(12)
    frame[36:36 + len(tag)] = tag
    return bytes(frame) + mp3()


def flac(padding=0):
    # 80 blocks * 4096 samples, 44.1 kHz, stereo, 16-bit. > 1 MiB
    # of encoded verbatim data exercises callback reads through ring wrap.
    count, block = 80, 4096
    frames = []
    for i in range(count):
        header = b"\xff\xf8\x79\x18" + bytes([i]) + struct.pack(">H", block - 1)
        header += bytes([crc(header, 8, 0x07)])
        frame = header + (b"\x02" + bytes(block * 2)) * 2
        frames.append(frame + struct.pack(">H", crc(frame, 16, 0x8005)))
    size = len(frames[0])
    info = struct.pack(">HH", block, block) + size.to_bytes(3, "big") * 2
    info += ((44100 << 44) | (1 << 41) | (15 << 36) | (count * block)).to_bytes(8, "big")
    info += bytes(16)  # unspecified PCM MD5, legal for a live encoder
    metadata = (b"\x00" if padding else b"\x80") + b"\x00\x00\x22" + info
    if padding:
        metadata += b"\x81" + padding.to_bytes(3, "big") + bytes(padding)
    return b"fLaC" + metadata + b"".join(frames)


def ogg_page(packet, sequence, granule, flags=0):
    lacing = bytes([255] * (len(packet) // 255) + [len(packet) % 255])
    header = b"OggS\x00" + bytes([flags]) + struct.pack("<QII", granule, 0x534f4e49, sequence)
    page = header + bytes(4) + bytes([len(lacing)]) + lacing + packet
    checksum = crc(page, 32, 0x04c11db7)
    return page[:22] + struct.pack("<I", checksum) + page[26:]


def opus():
    count, preskip, trim = 120, 312, 120
    head = b"OpusHead" + struct.pack("<BBHIhB", 1, 2, preskip, 48000, 0, 0)
    tags = b"OpusTags" + struct.pack("<I", len(b"morpthis")) + b"morpthis" + struct.pack("<I", 0)
    pages = [ogg_page(head, 0, 0, 2), ogg_page(tags, 1, 0)]
    for i in range(count):
        final = i == count - 1
        pages.append(ogg_page(b"\xf8\xff\xfe", i + 2, (i + 1) * 960 - (trim if final else 0), 4 if final else 0))
    return b"".join(pages)


def make_media():
    mp3_audio, flac_audio, opus_audio = mp3(), flac(), opus()
    return {"mp3": (mp3_audio, "audio/mpeg"),
            "mp3-id3": (mp3(700 * 1024), "audio/mpeg"),
            "mp3-xing": (mp3_xing(), "audio/mpeg"),
            "flac": (flac_audio, "audio/flac"),
            "flac-padding": (flac(700 * 1024), "audio/flac"),
            "opus": (opus_audio, "audio/ogg"),
            "codec-mismatch": (opus_audio, "audio/ogg")}
