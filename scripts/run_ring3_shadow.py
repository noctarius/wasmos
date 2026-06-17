#!/usr/bin/env python3
"""Configure and build a WASMOS ring3 shadow CMake tree, then run a target within it.

Replaces the two-command (cmake configure + cmake --build) inline chain in the
ring3 shadow test targets, making them a single COMMAND and runnable standalone.

Usage example (standalone iteration):
  python scripts/run_ring3_shadow.py \
    --cmake cmake \
    --source-dir . \
    --build-dir build/ring3 \
    --clang /opt/homebrew/opt/llvm/bin/clang \
    --lld ld.lld \
    --objcopy llvm-objcopy \
    --ovmf-code /path/to/OVMF_CODE.fd \
    --trace OFF \
    --define WASMOS_RING3_SMOKE=ON \
    --define WASMOS_PM_TEST_HOOKS=ON \
    --target run-qemu-ring3-check
"""

import argparse
import subprocess
import sys


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cmake",           required=True, help="Path to cmake executable")
    p.add_argument("--build-dir",       required=True, help="Build directory")
    p.add_argument("--target",          required=True, help="CMake target to build")
    p.add_argument("--skip-configure",  action="store_true",
                   help="Skip cmake configure; build-dir must already be configured")
    # Configure-only flags (ignored with --skip-configure)
    p.add_argument("--source-dir", default="", help="CMake source root (-S)")
    p.add_argument("--clang",      default="", help="Clang compiler path")
    p.add_argument("--lld",        default="", help="LLD linker name or path")
    p.add_argument("--objcopy",    default="", help="llvm-objcopy path")
    p.add_argument("--ovmf-code",  default="", help="OVMF_CODE.fd path")
    p.add_argument("--ovmf-vars",  default="", help="OVMF_VARS.fd path (optional)")
    p.add_argument("--trace",      default="OFF", help="WASMOS_TRACE value (ON or OFF)")
    p.add_argument("--define",     action="append", default=[], metavar="KEY=VALUE",
                   help="Extra -DKEY=VALUE flag for cmake configure (repeatable)")
    args = p.parse_args()

    cmds = []

    if not args.skip_configure:
        configure_cmd = [
            args.cmake,
            "-S", args.source_dir,
            "-B", args.build_dir,
            f"-DCLANG={args.clang}",
            f"-DLLD={args.lld}",
            f"-DOBJCOPY={args.objcopy}",
            f"-DOVMF_CODE={args.ovmf_code}",
            f"-DOVMF_VARS={args.ovmf_vars}",
            f"-DWASMOS_TRACE={args.trace}",
            "-DWASMOS_RING3_SHADOW=ON",
            "-DWASMOS_WARP_AOT_DISABLED=ON",
        ] + [f"-D{kv}" for kv in args.define]
        cmds.append(configure_cmd)

    cmds.append([args.cmake, "--build", args.build_dir, "--target", args.target])

    cwd = args.source_dir or args.build_dir
    for cmd in cmds:
        rc = subprocess.call(cmd, cwd=cwd)
        if rc != 0:
            return rc

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
