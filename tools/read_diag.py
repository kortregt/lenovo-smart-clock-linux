#!/usr/bin/env python3
"""Print the persistent boot diagnostics that initramfs/init writes into boot_b.

Usage:
    python3 tools/read_diag.py boot_b_readback.bin

The init writes a text block ("=== CLOCK-DIAG ..." through "=== END") at 16 MiB into
boot_b after every log line; the last write before a hang/reset is what survives.
"""
import sys

OFFSET = 16 << 20


def main():
    with open(sys.argv[1], "rb") as f:
        f.seek(OFFSET)
        blob = f.read(12 << 20)
    if not blob.startswith(b"=== CLOCK-DIAG"):
        sys.exit("no diag block at 16 MiB (init never reached boot_b, or wrong file)")
    end = blob.find(b"=== END")
    text = blob[: end + 7 if end >= 0 else blob.find(b"\0")]
    sys.stdout.write(text.decode("utf-8", "replace") + "\n")
    if end < 0:
        print("(no END marker: the write itself was interrupted)")


if __name__ == "__main__":
    main()
