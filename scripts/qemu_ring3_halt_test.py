#!/usr/bin/env python3
import argparse

from qemu_test_framework import QemuConfig, QemuSession, default_config


def main():
    parser = argparse.ArgumentParser(description="Run QEMU and assert ring3 smoke markers before halt.")
    parser.add_argument("--ovmf-code", default="")
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", default="")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    if args.ovmf_code or args.esp:
        cfg = QemuConfig(args.ovmf_code, args.ovmf_vars, args.esp)
    else:
        cfg = default_config()

    required = [
        b"[mode] strict-ring3=1",
        b"native-call-smoke: ipc-call ok",
        b"[test] ring3 native abi ok",
        b"[fault] user-pf pid=",
        b"reason=user_to_kernel",
        b"reason=write_violation",
        b"[test] ring3 fault isolate ok",
        b"[test] ring3 fault write reason ok",
        b"[test] ring3 fault exec reason ok",
        b"[test] ring3 fault ud reason ok",
        b"[test] ring3 fault gp reason ok",
        b"[test] ring3 fault de reason ok",
        b"[test] ring3 fault exit status ok",
        b"[test] ring3 fault write exit status ok",
        b"[test] ring3 fault exec exit status ok",
        b"[test] ring3 fault ud exit status ok",
        b"[test] ring3 fault gp exit status ok",
        b"[test] ring3 fault de exit status ok",
        b"[test] ring3 ipc syscall deny ok",
        b"[test] ring3 ipc syscall arg width deny ok",
        b"[test] ring3 ipc syscall control deny ok",
        b"[test] ring3 ipc syscall ok",
        b"[test] ring3 ipc call deny ok",
        b"[test] ring3 ipc call err rdx zero ok",
        b"[test] ring3 ipc call perm deny ok",
        b"[test] ring3 ipc call control deny ok",
        b"[test] ring3 ipc call ok",
        b"[test] ring3 ipc call correlate ok",
        b"[test] ring3 ipc call source auth ok",
        b"[test] pm wait reply owner deny ok",
        b"[test] pm kill owner deny ok",
        b"[test] pm status owner deny ok",
        b"[test] pm spawn owner deny ok",
        b"[test] ring3 yield syscall ok",
        b"[test] ring3 syscall ok",
        b"[test] ring3 preempt stress ok",
        b"wamos> ",
    ]

    with QemuSession(cfg, timeout_s=args.timeout) as session:
        for needle in required:
            if not session.expect(needle):
                return 1
        session.send("halt")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
