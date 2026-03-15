#!/usr/bin/env python3
import argparse
import shutil
import subprocess

from qemu_test_framework import QemuConfig, QemuSession, default_config

PREFERRED_DISPLAYERS = ["curses", "gtk", "sdl", "cocoa", "dbus"]


def detect_display_backend() -> str:
    qemu_bin = shutil.which("qemu-system-x86_64") or "qemu-system-x86_64"
    try:
        result = subprocess.run(
            [qemu_bin, "-display", "help"],
            capture_output=True,
            text=True,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError, PermissionError):
        return ""

    lines = [line.strip() for line in result.stdout.splitlines()]
    collecting = False
    available = []
    for line in lines:
        if not line:
            continue
        lower = line.lower()
        if lower.startswith("available display backend types"):
            collecting = True
            continue
        if collecting:
            if lower.startswith("some display"):
                break
            if line not in available:
                available.append(line)

    for preferred in PREFERRED_DISPLAYERS:
        if preferred in available:
            return preferred

    for candidate in available:
        if candidate and candidate.lower() != "none":
            return candidate

    return ""


def main():
    parser = argparse.ArgumentParser(description="Run QEMU with UI + serial smoke run.")
    parser.add_argument("--ovmf-code", default="")
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", default="")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument(
        "--display",
        default="auto",
        help="QEMU display backend to use (auto chooses a supported UI).",
    )
    args = parser.parse_args()

    if args.display in ("auto", "default"):
        args.display = detect_display_backend()

    if args.ovmf_code or args.esp:
        cfg = QemuConfig(
            args.ovmf_code,
            args.ovmf_vars,
            args.esp,
        )
    else:
        cfg = default_config()

    cfg.nographic = False
    cfg.display = args.display or ""

    with QemuSession(cfg, timeout_s=args.timeout) as session:
        if not session.expect(b"wamos> "):
            return 1
        session.send("halt")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
