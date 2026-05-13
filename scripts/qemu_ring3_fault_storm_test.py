#!/usr/bin/env python3
import argparse
from qemu_test_framework import QemuConfig, QemuSession, default_config


def marker_count(buf: bytes, needle: bytes) -> int:
    return buf.count(needle)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run QEMU and assert dedicated ring3 fault-storm liveness markers."
    )
    parser.add_argument("--ovmf-code", default="")
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", default="")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--attempts", type=int, default=3)
    args = parser.parse_args()

    if args.ovmf_code or args.esp:
        cfg = QemuConfig(args.ovmf_code, args.ovmf_vars, args.esp)
    else:
        cfg = default_config()

    required = [
        b"[kernel] kmain",
        b"[test] ring3 fault bp exit status ok",
        b"[test] ring3 containment liveness ok",
        b"[test] ring3 mixed stress ok",
        b"[test] ring3 watchdog clean ok",
        b"[test] sched progress ok",
    ]
    forbidden = [
        b"[test] ring3 mixed stress spawn failed",
        b"[test] ring3 mixed stress exit status mismatch",
        b"[test] ring3 mixed stress reap failed",
        b"[test] ring3 containment liveness mismatch",
        b"[test] ring3 watchdog clean mismatch",
        b"[watchdog] trap frame invalid cs=",
    ]

    max_attempts = args.attempts if args.attempts > 0 else 1
    for _ in range(max_attempts):
        with QemuSession(cfg, timeout_s=args.timeout) as session:
            for needle in required:
                if not session.expect(needle):
                    break
            else:
                for needle in forbidden:
                    if needle in session.buf:
                        break
                else:
                    # Require repeated mixed-fault churn after baseline probe set completes.
                    # Churn alternates #UD and #GP probes for several rounds and must produce
                    # multiple post-baseline reason markers to validate repeated fault load.
                    ud_reason = marker_count(session.buf, b"[test] ring3 fault ud reason ok")
                    gp_reason = marker_count(session.buf, b"[test] ring3 fault gp reason ok")
                    if ud_reason >= 2 and gp_reason >= 2:
                        session.send("halt")
                        return 0
            session.send("\x01x")

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
