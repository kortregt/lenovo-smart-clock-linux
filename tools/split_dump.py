#!/usr/bin/env python3
"""Split a full eMMC user-area dump into per-partition images using mtkclient's printgpt output.

Usage:
    uv run python tools/split_dump.py backup/full_user.bin backup/gpt.txt backup/parts [--include-userdata]

Writes one <name>.bin per partition, a SHA256SUMS file, and sanity-checks the images whose
format is known (GPT header, boot, vbmeta, lk, ext4), so a bad dump shows up here and not
after flashing something back.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

GPT_LINE = re.compile(r"^(\S+):\s+Offset (0x[0-9a-fA-F]+), Length (0x[0-9a-fA-F]+)")
CHUNK = 4 * 1024 * 1024


def parse_gpt(path):
    parts = []
    with open(path, errors="replace") as f:
        for line in f:
            m = GPT_LINE.match(line.strip())
            if m:
                parts.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    return parts


def check(name, head):
    """Return (ok, message) for partitions with a recognizable format, or None if unknown."""
    if name.startswith("boot_"):
        return head[:8] == b"ANDROID!", "Android boot image header"
    if name.startswith("vbmeta_"):
        return head[:4] == b"AVB0", "AVB vbmeta header"
    if name.startswith("lk_"):
        return head[:4] == struct.pack("<I", 0x58881688), "MTK image header"
    if name.startswith(("system_", "vendor_")):
        return head[0x438:0x43A] == b"\x53\xef", "ext4 superblock"
    if name in ("proinfo", "nvram", "seccfg"):
        blank = all(b in (0x00, 0xFF) for b in set(head))
        return not blank, "not blank"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("gpt")
    ap.add_argument("outdir")
    ap.add_argument("--include-userdata", action="store_true")
    args = ap.parse_args()

    parts = parse_gpt(args.gpt)
    if not parts:
        sys.exit(f"no partitions found in {args.gpt}")
    size = os.path.getsize(args.dump)
    os.makedirs(args.outdir, exist_ok=True)

    failures = 0
    with open(args.dump, "rb") as src:
        src.seek(0x200)
        gpt_ok = src.read(8) == b"EFI PART"
        print(f"{'OK ' if gpt_ok else 'BAD'} GPT header at 0x200")
        failures += not gpt_ok

        sums = []
        for name, off, length in parts:
            if name == "userdata" and not args.include_userdata:
                print(f"--  {name:<20} skipped (use --include-userdata)")
                continue
            if off + length > size:
                print(f"BAD {name:<20} extends past end of dump")
                failures += 1
                continue

            h = hashlib.sha256()
            src.seek(off)
            remaining = length
            head = b""
            with open(os.path.join(args.outdir, f"{name}.bin"), "wb") as dst:
                while remaining:
                    buf = src.read(min(CHUNK, remaining))
                    if not head:
                        head = buf[:0x1000]
                    h.update(buf)
                    dst.write(buf)
                    remaining -= len(buf)
            sums.append(f"{h.hexdigest()}  {name}.bin")

            result = check(name, head)
            if result is None:
                print(f"    {name:<20} {length:#x} bytes")
            else:
                ok, what = result
                failures += not ok
                print(f"{'OK ' if ok else 'BAD'} {name:<20} {length:#x} bytes, {what}")

    with open(os.path.join(args.outdir, "SHA256SUMS"), "w") as f:
        f.write("\n".join(sums) + "\n")

    print(f"\n{len(sums)} partitions written to {args.outdir}, {failures} check(s) failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
