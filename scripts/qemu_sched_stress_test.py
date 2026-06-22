#!/usr/bin/env python3
"""Boot a WASMOS kernel built with -DWASMOS_SCHED_SMP_STRESS and verify the
standalone SMP scheduler stress test passes.

This is a runtime-independent scheduler stability gate: it only watches for the
stress-test markers, so it works on any SMP build (wasm3 or WARP) and does not
depend on a bootable userspace. A pass requires the "[test] sched smp stress ok"
marker; a stalled ring (RUNNING-orphan / lost wakeup / stranded-ready thread)
prints "FAIL" and never emits "ok", so it is caught as a timeout/failure."""
import argparse
import os
import sys

from qemu_test_framework import QemuConfig, QemuSession, default_config


def main():
    parser = argparse.ArgumentParser(description="Run the SMP scheduler stress test.")
    parser.add_argument("--ovmf-code", default="")
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", default="")
    parser.add_argument("--userfs", default="")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--smp", type=int, default=4)
    args = parser.parse_args()

    if args.ovmf_code or args.esp:
        userfs = args.userfs or os.environ.get("WASMOS_USERFS", os.path.join(os.getcwd(), "userfs"))
        cfg = QemuConfig(args.ovmf_code, args.ovmf_vars, args.esp, userfs, smp_count=args.smp)
    else:
        cfg = default_config()

    with QemuSession(cfg, timeout_s=args.timeout) as session:
        # The stress test runs as the scheduler loop starts, well before (and
        # independent of) userspace bringup. "ok" is emitted only after every
        # worker completed its iteration quota with no orphaned/lost threads.
        if not session.expect(b"[test] sched smp stress ok"):
            sys.stderr.write("FAIL: SMP scheduler stress test did not pass "
                             "(stalled ring or never started)\n")
            return 1
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
