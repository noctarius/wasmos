#!/usr/bin/env python3
"""Copy a signed .wap with one byte of its payload changed.

For the signature policy's test (tests/test_signed_packages.py): the copy is
still a well-formed package whose signature no longer matches its bytes. The
byte changed is the middle one of everything the signature covers, which lands
in the module payload rather than the header, so the copy still parses.

Usage: tamper_byte.py <in.wap> <out.wap>
"""

import struct
import sys

# Offset of the header's signature_size: the length of the signature record that
# ends a signed package (wasmos_app_header_t, src/kernel/wasmos_app.c).
SIGNATURE_SIZE_OFFSET = 56


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())
    if len(data) < SIGNATURE_SIZE_OFFSET + 4:
        print(f"{src}: too small to be a package", file=sys.stderr)
        return 1
    (record,) = struct.unpack_from("<I", data, SIGNATURE_SIZE_OFFSET)
    if record == 0 or len(data) <= record * 2:
        print(f"{src}: not a signed package", file=sys.stderr)
        return 1
    data[(len(data) - record) // 2] ^= 0x01
    open(dst, "wb").write(bytes(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
