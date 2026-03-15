#!/usr/bin/env python3
import argparse
import shutil
import subprocess
import sys
import time

from qemu_test_framework import QemuConfig, QemuSession, default_config

PREFERRED_DISPLAYERS = ["cocoa", "gtk", "sdl", "dbus", "curses"]


def _available_display_backends() -> list[str]:
    qemu_bin = shutil.which("qemu-system-x86_64") or "qemu-system-x86_64"
    try:
        result = subprocess.run(
            [qemu_bin, "-display", "help"],
            capture_output=True,
            text=True,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError, PermissionError):
        return []

    lines = [line.strip().lower() for line in result.stdout.splitlines()]
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
    return available


def detect_display_backend() -> str:
    available = _available_display_backends()
    if not available:
        return ""

    has_terminal = sys.stdin.isatty() and sys.stdout.isatty()
    for preferred in PREFERRED_DISPLAYERS:
        if preferred not in available:
            continue
        if preferred == "curses" and not has_terminal:
            continue
        return preferred

    if not has_terminal and "none" in available:
        return "none"

    for candidate in available:
        if candidate and candidate.lower() != "none":
            return candidate

    return "none" if "none" in available else ""


def main():
    parser = argparse.ArgumentParser(description="Run QEMU with UI + serial smoke run.")
    parser.add_argument("--ovmf-code", default="")
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", default="")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument(
        "--hold-time",
        type=float,
        default=0.0,
        help="Seconds to wait after the shell prompt before sending `halt`.",
    )
    parser.add_argument(
        "--no-halt",
        action="store_true",
        help="Do not send `halt`; keep the machine running until the user stops it.",
    )
    parser.add_argument(
        "--display",
        default="auto",
        help="QEMU display backend to use (auto chooses a supported UI).",
    )
    args = parser.parse_args()

    display_backend = args.display
    if display_backend in ("auto", "default"):
        display_backend = detect_display_backend()

    if args.ovmf_code or args.esp:
        cfg = QemuConfig(
            args.ovmf_code,
            args.ovmf_vars,
            args.esp,
        )
    else:
        cfg = default_config()

    if not display_backend or display_backend == "none":
        cfg.nographic = True
        cfg.display = ""
    else:
        cfg.nographic = False
        cfg.display = display_backend

    with QemuSession(cfg, timeout_s=args.timeout) as session:
        if not session.expect(b"wamos> "):
            return 1
        if args.hold_time > 0:
            time.sleep(args.hold_time)
        if not args.no_halt:
            session.send("halt")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
