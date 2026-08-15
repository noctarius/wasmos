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

OK_MARKER = b"[test] sched smp stress ok"

# Exit status. A kernel with no stress test compiled in reaches the timeout with
# no marker, which is byte-identical to a stalled ring; the preflight below turns
# that into its own status so the two are never confused again.
EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_NOT_BUILT = 2


def stress_test_compiled_in(esp_dir):
    """True when the ESP's kernel can emit the stress markers at all.

    kernel_sched_smp_stress_runtime.c compiles to a no-op stub without
    WASMOS_SCHED_SMP_STRESS, so the marker string is absent from the image.
    Returns None when the kernel cannot be located or read -- the caller then
    boots anyway rather than refusing on a check it could not perform.
    """
    kernel = os.path.join(esp_dir, "kernel.elf")
    try:
        with open(kernel, "rb") as f:
            return OK_MARKER in f.read()
    except OSError:
        return None


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
        userfs = args.userfs or os.environ.get(
            "WASMOS_USERFS", os.path.join(os.getcwd(), "userfs")
        )
        cfg = QemuConfig(
            args.ovmf_code, args.ovmf_vars, args.esp, userfs, smp_count=args.smp
        )
    else:
        cfg = default_config()

    if stress_test_compiled_in(cfg.esp_dir) is False:
        sys.stderr.write(
            "SKIP: this kernel has no SMP scheduler stress test compiled in, so "
            "the gate cannot run.\n"
            "It is OFF by default. Configure a tree that enables it:\n"
            "  cmake -S . -B build-sched-stress "
            "-DWASMOS_DOTCONFIG=configs/warp_smp_defconfig "
            "-DWASMOS_SCHED_SMP_STRESS=ON\n"
            "  cmake --build build-sched-stress "
            "--target run-qemu-sched-stress-test\n"
        )
        return EXIT_NOT_BUILT

    with QemuSession(cfg, timeout_s=args.timeout) as session:
        # The stress test runs as the scheduler loop starts, well before (and
        # independent of) userspace bringup. "ok" is emitted only after every
        # worker completed its iteration quota with no orphaned/lost threads.
        if not session.expect(OK_MARKER):
            sys.stderr.write(
                "FAIL: SMP scheduler stress test did not pass (stalled ring)\n"
            )
            return EXIT_FAIL
        return EXIT_PASS


if __name__ == "__main__":
    raise SystemExit(main())
