#!/usr/bin/env python3
import argparse
import os
import sys

from qemu_test_framework import QemuConfig, QemuSession, default_config


def main():
    parser = argparse.ArgumentParser(description="Run QEMU and halt via CLI.")
    parser.add_argument("--ovmf-code", default="")
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", default="")
    parser.add_argument("--userfs", default="")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--smp", type=int, default=1)
    args = parser.parse_args()

    if args.ovmf_code or args.esp:
        userfs = args.userfs or os.environ.get("WASMOS_USERFS", os.path.join(os.getcwd(), "userfs"))
        cfg = QemuConfig(args.ovmf_code, args.ovmf_vars, args.esp, userfs, smp_count=args.smp)
    else:
        cfg = default_config()

    with QemuSession(cfg, timeout_s=args.timeout) as session:
        # Calculator is spawned by sysinit.rc before the CLI starts.
        # Its startup log appears in the serial stream before "wamos>".
        if not session.expect(b"[calculator] start"):
            sys.stderr.write("FAIL: calculator did not start (WARP JIT failure?)\n")
            return 1
        # Wait for CLI
        if not session.expect(b"wamos> "):
            return 1
        session.send("halt")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
