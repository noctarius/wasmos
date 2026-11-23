#!/usr/bin/env python3
"""Boot a tree until a marker appears on the console, then stop.

The gate for an architecture under bring-up, whose kernel cannot reach the CLI
that run-qemu-test drives: it proves the loader, the kernel entry and kmain's
first steps on that architecture. Exit 0 when the marker appears within the
timeout, 1 otherwise (with the console tail). The guest architecture and QEMU
binary come from WASMOS_ARCH / WASMOS_QEMU, which the run targets set."""

import argparse
import sys

from qemu_test_framework import GuestPanicked, QemuConfig, QemuSession


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ovmf-code", required=True)
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", required=True)
    parser.add_argument("--marker", required=True)
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()

    cfg = QemuConfig(args.ovmf_code, args.ovmf_vars, args.esp)
    marker = args.marker.encode()
    with QemuSession(cfg, timeout_s=args.timeout) as session:
        try:
            reached = session.expect(marker, timeout_s=args.timeout)
        except GuestPanicked:
            # A panic later in the boot -- the next unimplemented piece of an
            # architecture under bring-up -- does not undo reaching the marker.
            reached = marker in session.buf
        if not reached:
            sys.stderr.write(
                f"FAIL: '{args.marker}' did not appear within {args.timeout}s\n"
            )
            sys.stderr.write(session.tail() + "\n")
            return 1
        print(f"[bringup] reached '{args.marker}'")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
