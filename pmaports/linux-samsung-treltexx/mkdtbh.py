#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pack DTBs into a Samsung Exynos "DTBH" v2 dt.img (S-Boot, Galaxy Note 4).

S-Boot picks the blob whose chip/platform/subtype/hw_rev match the board.
The platform/subtype codes are fixed numbers for this bootloader generation
(dtbTool-exynos in pmOS derives hashes instead, which S-Boot here rejects).

usage: mkdtbh.py -o dt.img --chip 5433 --platform 0x3ec3 \
                 --subtype 0x3a37307e [--rev 0:255] [--pagesize 2048] a.dtb...
"""
import argparse
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--chip", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--platform", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--subtype", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--rev", default="0:255")
    ap.add_argument("--pagesize", type=int, default=2048)
    ap.add_argument("dtbs", nargs="+")
    a = ap.parse_args()

    rev, rev_end = (int(x, 0) for x in a.rev.split(":"))
    pg = a.pagesize
    pad = lambda n: (n + pg - 1) // pg * pg

    blobs = [open(p, "rb").read() for p in a.dtbs]
    hdr_sz = pad(12 + 32 * len(blobs))
    out = bytearray(struct.pack("<4sII", b"DTBH", 2, len(blobs)))
    off = hdr_sz
    for b in blobs:
        size = pad(len(b))
        out += struct.pack("<8I", a.chip, a.platform, a.subtype, rev, rev_end,
                           off, size, 0x20)
        off += size
    out += bytes(hdr_sz - len(out))
    for b in blobs:
        out += b + bytes(pad(len(b)) - len(b))
    open(a.output, "wb").write(out)


if __name__ == "__main__":
    main()
