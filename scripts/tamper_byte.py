#!/usr/bin/env python3
"""Copy a signed .wap with one byte of its payload changed.

For the signature policy's test (tests/test_signed_packages.py): the copy is
still a well-formed package whose signature no longer matches its bytes. The
byte changed is the middle one of everything the signature covers, which lands
in the module payload rather than the header, so the copy still parses.

Usage: tamper_byte.py <in.wap> <out.wap>
"""

import sys

SIGNATURE_LEN = 64


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())
    if len(data) <= SIGNATURE_LEN * 2:
        print(f"{src}: too small to be a signed package", file=sys.stderr)
        return 1
    data[(len(data) - SIGNATURE_LEN) // 2] ^= 0x01
    open(dst, "wb").write(bytes(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
