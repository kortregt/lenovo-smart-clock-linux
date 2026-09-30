#!/usr/bin/env python3
"""Read or build the A/B slot control block in the Smart Clock's misc partition.

The bootloader keeps slot state at misc+0x800 in the old libavb "\\0AB0" format:
magic, version 1.0, then per slot {priority, tries_remaining, successful_boot, reserved},
12 reserved bytes and a big-endian CRC32 of the first 28 bytes.

Usage:
    python3 tools/ab_control.py decode misc.bin            # a misc dump (or a 32-byte block)
    python3 tools/ab_control.py build --prefer b out.bin    # write a 32-byte block

On the clock, write the block with:
    dd if=out.bin of=/dev/<misc> bs=1 seek=2048 count=32 conv=notrunc,fsync
"""
import argparse
import struct
import sys
import zlib

OFFSET = 0x800
MAGIC = b"\0AB0"


def decode(blob):
    if len(blob) > 32:
        blob = blob[OFFSET:OFFSET + 32]
    if blob[:4] != MAGIC:
        sys.exit("no \\0AB0 block found")
    crc_ok = struct.unpack(">I", blob[28:32])[0] == zlib.crc32(blob[:28])
    print(f"version {blob[4]}.{blob[5]}, crc {'ok' if crc_ok else 'BAD'}")
    for name, off in (("A", 8), ("B", 12)):
        pri, tries, ok = blob[off], blob[off + 1], blob[off + 2]
        print(f"slot {name}: priority {pri}, tries {tries}, successful {ok}"
              + ("  (unbootable)" if pri == 0 else ""))


def build(prefer):
    hi, lo = (15, 1, 0), (14, 1, 0)  # priority, successful, tries
    a, b = (lo, hi) if prefer == "b" else (hi, lo)
    blob = MAGIC + bytes([1, 0, 0, 0])
    for pri, ok, tries in (a, b):
        blob += bytes([pri, tries, ok, 0])
    blob += bytes(12)
    return blob + struct.pack(">I", zlib.crc32(blob))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decode")
    d.add_argument("file")
    b = sub.add_parser("build")
    b.add_argument("--prefer", choices=("a", "b"), required=True)
    b.add_argument("out")
    args = ap.parse_args()
    if args.cmd == "decode":
        decode(open(args.file, "rb").read())
    else:
        blob = build(args.prefer)
        open(args.out, "wb").write(blob)
        decode(blob)


if __name__ == "__main__":
    main()
