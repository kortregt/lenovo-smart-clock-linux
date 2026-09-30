#!/usr/bin/env python3
"""Swap the kernel in an Android v0 boot image, keeping everything else from the original.

Usage:
    python3 tools/repack_boot.py backup/parts/boot_a.bin Image.gz mt8167s_ref.dtb out/boot_new.img \
        [--ramdisk initramfs.cpio.gz] [--cmdline "..."]

The kernel is written as Image.gz with the DTB appended (the Image.gz-dtb layout the stock
lk expects). Header fields (load addresses, page size, os_version, name) and the ramdisk
and second-stage payloads are copied unchanged, unless --ramdisk/--cmdline replace them;
the id field is recomputed like mkbootimg.
The original partition's AVB footer is dropped, which is fine only while AVB is unlocked.
"""
import argparse
import hashlib
import struct
import sys

MAGIC = b"ANDROID!"
HDR = struct.Struct("<8s10I16s512s32s1024s")


def pad(data, page):
    return data + b"\0" * (-len(data) % page)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("orig")
    ap.add_argument("kernel_gz")
    ap.add_argument("dtb")
    ap.add_argument("out")
    ap.add_argument("--ramdisk", help="replace the ramdisk (default: keep original)")
    ap.add_argument("--cmdline", help="replace the header cmdline (default: keep original)")
    a = ap.parse_args()

    d = open(a.orig, "rb").read()
    (magic, ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, hver, osver,
     name, cmdline, _id, extra) = HDR.unpack_from(d)
    if magic != MAGIC or hver != 0:
        sys.exit("not an Android v0 boot image")

    off = page
    ramdisk = d[off + -(-ksz // page) * page:][:rsz]
    off += -(-ksz // page) * page + -(-rsz // page) * page
    second = d[off:off + ssz]

    kernel = open(a.kernel_gz, "rb").read() + open(a.dtb, "rb").read()
    if kernel[:2] != b"\x1f\x8b":
        sys.exit("kernel is not gzip (pass Image.gz, not Image)")
    if a.ramdisk:
        ramdisk = open(a.ramdisk, "rb").read()
        rsz = len(ramdisk)
    if a.cmdline is not None:
        cmdline = a.cmdline.encode().ljust(512, b"\0")[:512]

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    new_id = sha.digest().ljust(32, b"\0")

    hdr = HDR.pack(MAGIC, len(kernel), kaddr, rsz, raddr, ssz, saddr, tags, page, hver,
                   osver, name, cmdline, new_id, extra)
    img = pad(hdr, page) + pad(kernel, page) + pad(ramdisk, page) + (pad(second, page) if ssz else b"")
    open(a.out, "wb").write(img)
    print(f"{a.out}: {len(img)} bytes, kernel {len(kernel)}, ramdisk {rsz}, "
          f"cmdline {cmdline.rstrip(chr(0).encode()).decode()!r}")


if __name__ == "__main__":
    main()
