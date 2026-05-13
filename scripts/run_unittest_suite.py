#!/usr/bin/env python3
import argparse
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description="Run unittest suite with deterministic status markers.")
    parser.add_argument("--start-dir", default="tests")
    args = parser.parse_args()

    cmd = [sys.executable, "-m", "unittest", "discover", "-s", args.start_dir]
    rc = subprocess.call(cmd)
    if rc == 0:
        print("[test] cli suite status ok")
        return 0
    print("[test] cli suite status failed")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
