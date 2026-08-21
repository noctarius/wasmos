#!/usr/bin/env python3
"""Validate that a WASM module's stack pointer and data segments stay inside a
layout budget.

What this checks is that a small shadow stack was actually applied. A module
built with a toolchain's default -- 1 MB for both Zig and rustc, placed first --
puts its globals above 1 MB and must declare at least 2 MiB of linear memory,
where the same module with an 8 KiB stack fits a single 64 KiB page like a C one.
Tripping this check means the small stack did not take effect.

It is NOT a pointer-validity check, though it was documented as one for a long
time. The claim was that a fixed 16-page MEM_REGION_WASM_LINEAR mirror bounds
every pointer a host call receives; reserved-VA linmem made that false, because
mm_context_rebind_wasm_linear and mm_context_bind_wasm_linear_scattered resize
that region to the guest's real linear memory. Measured on 2026-08-21: a Zig
module with data at 0x100000 -- which fails this check -- runs console_write and
xfer_buffer_read successfully under both WARP and wasm3. See docs/TASKS.md."""

import argparse
import sys


def read_leb128(data, pos):
    result, shift = 0, 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            break
    return result, pos


def check_wasm(path, stack_size, max_addr):
    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x00asm":
        print(f"error: {path}: not a WASM file", file=sys.stderr)
        return False

    pos = 8
    stack_ptr = None
    data_end = 0
    errors = []

    while pos < len(data):
        section_id = data[pos]
        pos += 1
        size, pos = read_leb128(data, pos)
        end = pos + size

        if section_id == 6:  # Global section
            count, p = read_leb128(data, pos)
            for _ in range(count):
                _vtype = data[p]
                p += 1
                _mutable = data[p]
                p += 1
                instr = data[p]
                p += 1
                if instr == 0x41:  # i32.const
                    val, p = read_leb128(data, p)
                    p += 1  # end opcode
                    if stack_ptr is None:
                        stack_ptr = val
                    break

        elif section_id == 11:  # Data section
            count, p = read_leb128(data, pos)
            for _ in range(count):
                _flags, p = read_leb128(data, p)
                instr = data[p]
                p += 1
                if instr == 0x41:  # i32.const offset
                    offset, p = read_leb128(data, p)
                    p += 1  # end opcode
                    seg_len, p = read_leb128(data, p)
                    seg_end = offset + seg_len
                    if seg_end > max_addr:
                        errors.append(
                            f"  data segment at 0x{offset:x}+{seg_len} "
                            f"ends at 0x{seg_end:x} > limit 0x{max_addr:x}"
                        )
                    data_end = max(data_end, seg_end)
                    p += seg_len

        pos = end

    if stack_ptr is None:
        errors.append("  no stack pointer global found")
    elif stack_ptr != stack_size:
        errors.append(
            f"  stack pointer is 0x{stack_ptr:x} ({stack_ptr}), "
            f"expected 0x{stack_size:x} ({stack_size}) -- "
            f"rebuild with --stack {stack_size}"
        )
    elif stack_ptr > max_addr:
        errors.append(
            f"  stack pointer 0x{stack_ptr:x} exceeds user-VA limit 0x{max_addr:x}"
        )

    if errors:
        print(f"WASM stack check FAILED: {path}", file=sys.stderr)
        for e in errors:
            print(e, file=sys.stderr)
        return False

    print(
        f"wasm_stack_check ok: {path}: "
        f"stack_ptr=0x{stack_ptr:x} data_end=0x{data_end:x} limit=0x{max_addr:x}"
    )
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Validate Zig WASM utility stack and data layout"
    )
    parser.add_argument("wasm", help="Path to .wasm file")
    parser.add_argument(
        "--stack-size",
        type=int,
        required=True,
        help="Expected stack pointer value (must match --stack N passed to zig build-exe)",
    )
    parser.add_argument(
        "--max-addr",
        type=int,
        required=True,
        help="Maximum linear address allowed (kernel user-VA region size in bytes)",
    )
    args = parser.parse_args()

    if not check_wasm(args.wasm, args.stack_size, args.max_addr):
        sys.exit(1)


if __name__ == "__main__":
    main()
