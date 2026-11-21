#!/usr/bin/env python3
"""Populate an ESP directory from a staging plan and write a stamp.

The plan is JSON written by the build generator:

    {"files":    [[dest, src], ...],   # copy src to <esp>/dest
     "trees":    [[dest, srcdir], ...],# replace <esp>/dest with a copy of srcdir
     "optional": [[dest, src], ...],   # copy src if it exists, else an empty file
     "dirs":     [dest, ...]}          # directories that exist even when empty

Files are copied only when their content differs, so an unchanged ESP keeps
its timestamps. Entries that are not in the plan are left alone: a test may
have written them, and QEMU's fat:rw: backend writes NvVars.
"""

from __future__ import annotations

import argparse
import filecmp
import json
import shutil
import sys
from pathlib import Path


def copy_if_different(src: Path, dst: Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() and filecmp.cmp(src, dst, shallow=False):
        return
    shutil.copyfile(src, dst)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", type=Path, required=True)
    ap.add_argument("--esp", type=Path, required=True)
    ap.add_argument("--stamp", type=Path, required=True)
    args = ap.parse_args()

    plan = json.loads(args.plan.read_text())
    esp = args.esp
    esp.mkdir(parents=True, exist_ok=True)
    for dest in plan.get("dirs", []):
        (esp / dest).mkdir(parents=True, exist_ok=True)
    for dest, src in plan.get("files", []):
        copy_if_different(Path(src), esp / dest)
    for dest, src in plan.get("trees", []):
        target = esp / dest
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(src, target)
    for dest, src in plan.get("optional", []):
        if Path(src).is_file():
            copy_if_different(Path(src), esp / dest)
        else:
            (esp / dest).parent.mkdir(parents=True, exist_ok=True)
            (esp / dest).write_bytes(b"")
    args.stamp.write_text("staged\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
