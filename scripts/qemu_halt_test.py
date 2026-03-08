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
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    if args.ovmf_code or args.esp:
        cfg = QemuConfig(args.ovmf_code, args.ovmf_vars, args.esp)
    else:
        cfg = default_config()

    with QemuSession(cfg, timeout_s=args.timeout) as session:
        if not session.expect(b"wamos> "):
            return 1
        session.send("halt")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
