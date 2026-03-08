#!/usr/bin/env python3
import argparse
import os
import selectors
import subprocess
import sys
import time


def build_qemu_cmd(ovmf_code, ovmf_vars, esp_dir):
    cmd = [
        "qemu-system-x86_64",
        "-m",
        "512M",
        "-nographic",
        "-serial",
        "mon:stdio",
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
    ]
    if ovmf_vars:
        cmd += ["-drive", f"if=pflash,format=raw,file={ovmf_vars}"]
    cmd += ["-drive", f"format=raw,file=fat:rw:{esp_dir}"]
    return cmd


def run_test(cmd, timeout_s):
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )

    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)

    buf = b""
    sent_halt = False
    deadline = time.time() + timeout_s

    while True:
        if proc.poll() is not None:
            break
        if time.time() > deadline:
            break

        events = selector.select(timeout=0.1)
        for key, _ in events:
            chunk = key.fileobj.read(4096)
            if not chunk:
                continue
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            buf += chunk
            if not sent_halt and b"wasmos> " in buf:
                try:
                    proc.stdin.write(b"halt\r\n")
                    proc.stdin.flush()
                    sent_halt = True
                except BrokenPipeError:
                    pass

        if sent_halt and proc.poll() is not None:
            break

    if proc.poll() is None:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    return 0 if sent_halt and proc.returncode == 0 else 1


def main():
    parser = argparse.ArgumentParser(description="Run QEMU and halt via CLI.")
    parser.add_argument("--ovmf-code", required=True)
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", required=True)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    if not os.path.exists(args.ovmf_code):
        print(f"OVMF code not found: {args.ovmf_code}", file=sys.stderr)
        return 1
    if args.ovmf_vars and not os.path.exists(args.ovmf_vars):
        print(f"OVMF vars not found: {args.ovmf_vars}", file=sys.stderr)
        return 1
    if not os.path.isdir(args.esp):
        print(f"ESP dir not found: {args.esp}", file=sys.stderr)
        return 1

    cmd = build_qemu_cmd(args.ovmf_code, args.ovmf_vars, args.esp)
    return run_test(cmd, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
