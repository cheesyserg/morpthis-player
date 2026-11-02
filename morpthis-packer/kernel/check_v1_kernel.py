#!/usr/bin/env python3
"""Checks a replacement TempoTec V1 kernel before the packer puts it in v1.upt.

    check_v1_kernel.py PIN.json CANDIDATE_XIMAGE STOCK_XIMAGE

The candidate is accepted only when every one of these holds:

  * its SHA-256 and size are the ones in the pin (kernel/tempotec_v1.json,
    taken from compas-firmware's firmware/kernel/tempotec_v1.json, which
    records the source commit, configuration and build of the image);
  * it is no larger than the stock kernel, so it fits the partition the
    stock update writes it to;
  * it is a well-formed legacy uImage -- magic, header CRC, payload length and
    data CRC -- for Linux on MIPS, of the kernel type;
  * the device tree embedded in it is byte for byte the one embedded in the
    stock kernel of the image being repacked, and its hash is the pinned one.
    The device tree describes the board: a kernel carrying any other would be
    a kernel for some other board.

Prints one line of facts on success, exits non-zero with the reason otherwise.
Only the Python standard library is used, so it runs wherever the packer does.
"""

from __future__ import annotations

import hashlib
import json
import re
import struct
import sys
import zlib


def fail(message: str) -> None:
    print(f"check_v1_kernel: {message}", file=sys.stderr)
    sys.exit(1)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def check_uimage(data: bytes, label: str) -> None:
    if len(data) < 64:
        fail(f"{label} is shorter than a uImage header")
    magic, hcrc, _time, size, _load, _entry, dcrc, os_id, arch, kind, _comp = struct.unpack(
        ">7I4B", data[:32])[:11]
    header = bytearray(data[:64])
    header[4:8] = b"\0\0\0\0"
    if magic != 0x27051956 or (zlib.crc32(bytes(header)) & 0xFFFFFFFF) != hcrc:
        fail(f"{label}: uImage magic or header CRC is wrong")
    if size != len(data) - 64 or (zlib.crc32(data[64:]) & 0xFFFFFFFF) != dcrc:
        fail(f"{label}: uImage payload length or data CRC is wrong")
    if (os_id, arch, kind) != (5, 5, 2):
        fail(f"{label}: not a Linux/MIPS kernel uImage (os {os_id}, arch {arch}, type {kind})")


def embedded_dtb(data: bytes, label: str) -> bytes:
    """The flattened device tree appended to the compressed kernel."""
    offset = data.find(b"\x1f\x8b\x08")
    if offset < 0:
        fail(f"{label}: no gzip stream in the image")
    try:
        raw = zlib.decompressobj(31).decompress(data[offset:])
    except zlib.error as exc:
        fail(f"{label}: the kernel does not decompress ({exc})")
    for match in re.finditer(b"\xd0\x0d\xfe\xed", raw):
        size = struct.unpack(">I", raw[match.start() + 4:match.start() + 8])[0]
        if 1000 < size < 200000:
            return raw[match.start():match.start() + size]
    fail(f"{label}: no embedded device tree")
    return b""


def main() -> None:
    if len(sys.argv) != 4:
        fail("usage: check_v1_kernel.py PIN.json CANDIDATE_XIMAGE STOCK_XIMAGE")
    pin_path, candidate_path, stock_path = sys.argv[1:]
    try:
        with open(pin_path, encoding="utf-8") as handle:
            pin = json.load(handle)
    except (OSError, ValueError) as exc:
        fail(f"cannot read the pin {pin_path}: {exc}")
    if pin.get("schema_version") != 1 or pin.get("board") != "tempotec_v1":
        fail("the pin is not a schema 1 tempotec_v1 kernel pin")
    for key in ("sha256", "dtb_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", str(pin.get(key, ""))):
            fail(f"the pin has no valid {key}")

    with open(candidate_path, "rb") as handle:
        candidate = handle.read()
    with open(stock_path, "rb") as handle:
        stock = handle.read()

    if sha256(candidate) != pin["sha256"]:
        fail(f"{candidate_path} has SHA-256 {sha256(candidate)}, the pin says {pin['sha256']}")
    if len(candidate) != pin.get("size_bytes"):
        fail(f"{candidate_path} is {len(candidate)} bytes, the pin says {pin.get('size_bytes')}")
    if len(candidate) > len(stock):
        fail(f"the replacement kernel ({len(candidate)} bytes) is larger than the stock one ({len(stock)})")

    check_uimage(candidate, "replacement kernel")
    check_uimage(stock, "stock kernel")

    candidate_dtb = embedded_dtb(candidate, "replacement kernel")
    stock_dtb = embedded_dtb(stock, "stock kernel")
    if sha256(candidate_dtb) != pin["dtb_sha256"]:
        fail("the replacement kernel's device tree is not the pinned one")
    if candidate_dtb != stock_dtb:
        fail("the replacement kernel's device tree differs from the stock kernel's: "
             "this stock firmware is not the board the kernel was built for")

    print(f"kernel {pin.get('kernel_release', '?')} from {pin.get('source_repo', '?')}@"
          f"{str(pin.get('source_commit', '?'))[:12]}, {len(candidate)} bytes (stock {len(stock)}), "
          f"device tree {pin['dtb_sha256'][:12]} matches the stock kernel")


if __name__ == "__main__":
    main()
