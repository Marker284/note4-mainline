#!/usr/bin/env python3
"""Turn downstream drivers/sensorhub/brcm/bbdpl/bbd_patch_file.h into the
firmware file served by /dev/bbd_patch (bbd.ko, request_firmware()).

Downstream did   static unsigned char bbd_new_patch[] = { #include "bbd_patch_file.h" };
i.e. one string literal made of concatenated pieces; sizeof() included the
terminating NUL, and LHD read exactly those bytes.  This script reproduces
them byte for byte (use --no-nul to drop the trailing NUL).

usage: extract_bbd_patch.py bbd_patch_file.h bcm_sensorhub_a2.patch
install: /lib/firmware/bcm4773/bcm_sensorhub_a2.patch
"""
import re
import sys

ESC = {'n': b'\n', 't': b'\t', 'r': b'\r', '0': b'\0', '\\': b'\\',
       '"': b'"', "'": b"'", 'a': b'\a', 'b': b'\b', 'f': b'\f', 'v': b'\v'}


def unescape(s):
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c != '\\':
            out += c.encode('latin-1')
            i += 1
            continue
        n = s[i + 1]
        if n == 'x':
            m = re.match(r'[0-9a-fA-F]+', s[i + 2:])
            out.append(int(m.group(0), 16) & 0xff)
            i += 2 + len(m.group(0))
        elif n in '01234567':
            m = re.match(r'[0-7]{1,3}', s[i + 1:])
            out.append(int(m.group(0), 8) & 0xff)
            i += 1 + len(m.group(0))
        else:
            out += ESC[n]
            i += 2
    return bytes(out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        sys.exit(__doc__)
    src = open(args[0], encoding='latin-1').read()
    # drop comments, keep string literals
    data = b''.join(unescape(m.group(1))
                    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', src))
    if '--no-nul' not in sys.argv:
        data += b'\0'
    open(args[1], 'wb').write(data)
    print(f'{args[1]}: {len(data)} bytes')


if __name__ == '__main__':
    main()
