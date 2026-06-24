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
        # Fast fail: the kernel prints "[kernel] boot_info version=" immediately
        # after it accepts boot_info at kmain. If that early line does not appear
        # within a few seconds the boot is dead (e.g. it halted on
        # "[kernel] invalid boot_info"); don't wait out the whole timeout.
        if not session.expect(b"[kernel] boot_info version=", timeout_s=20):
            sys.stderr.write("FAIL: kernel did not reach early boot within 20s (boot is dead)\n")
            sys.stderr.write(session.tail() + "\n")
            return 1
        # Calculator is spawned by sysinit.rc before the CLI starts.
        # Wait for full initialisation (window created, first render done),
        # not just the early start log — catching that alone would miss
        # crashes that happen during ui_init or the first drain().
        if not session.expect(b"[calculator] ready"):
            sys.stderr.write("FAIL: calculator did not fully initialise\n")
            sys.stderr.write(session.tail() + "\n")
            return 1
        # Wait for CLI
        if not session.expect(b"wamos> "):
            return 1
        session.send("halt")
        if not session.wait_for_exit(5):
            sys.stderr.write("FAIL: halt did not power off QEMU\n")
            return 1
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
