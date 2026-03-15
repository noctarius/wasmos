#!/usr/bin/env python3
import argparse
import os
import re
import selectors
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Optional, Pattern, Union


@dataclass
class QemuConfig:
    ovmf_code: str
    ovmf_vars: str
    esp_dir: str
    nographic: bool = True
    display: str = ""


def _read_cmake_cache(cache_path: str) -> dict:
    data = {}
    if not os.path.exists(cache_path):
        return data
    with open(cache_path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("//") or line.startswith("#"):
                continue
            if ":" not in line or "=" not in line:
                continue
            key, rest = line.split(":", 1)
            _, value = rest.split("=", 1)
            data[key] = value
    return data


def default_config(build_dir: str = "build") -> QemuConfig:
    cache = _read_cmake_cache(os.path.join(build_dir, "CMakeCache.txt"))
    ovmf_code = os.environ.get("WASMOS_OVMF_CODE", cache.get("OVMF_CODE", ""))
    ovmf_vars = os.environ.get("WASMOS_OVMF_VARS", cache.get("OVMF_VARS", ""))
    esp_dir = os.environ.get("WASMOS_ESP", os.path.join(build_dir, "esp"))
    if not ovmf_code:
        raise RuntimeError("OVMF_CODE not set (WASMOS_OVMF_CODE or CMakeCache.txt)")
    return QemuConfig(ovmf_code=ovmf_code, ovmf_vars=ovmf_vars, esp_dir=esp_dir)


def build_qemu_cmd(cfg: QemuConfig) -> list:
    cmd = [
        "qemu-system-x86_64",
        "-m",
        "512M",
        "-serial",
        "mon:stdio",
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={cfg.ovmf_code}",
    ]
    if cfg.nographic:
        cmd += ["-nographic"]
    else:
        if cfg.display:
            cmd += ["-display", cfg.display]
    if cfg.ovmf_vars:
        cmd += ["-drive", f"if=pflash,format=raw,file={cfg.ovmf_vars}"]
    cmd += ["-drive", f"format=raw,file=fat:rw:{cfg.esp_dir}"]
    return cmd


class QemuSession:
    def __init__(self, cfg: QemuConfig, timeout_s: int = 120, echo: bool = True,
                 force_stop_on_timeout: bool = True):
        self.cfg = cfg
        self.timeout_s = timeout_s
        self.echo = echo
        self.force_stop_on_timeout = force_stop_on_timeout
        self.proc: Optional[subprocess.Popen] = None
        self.selector: Optional[selectors.BaseSelector] = None
        self.buf = b""

    def start(self) -> None:
        if not os.path.exists(self.cfg.ovmf_code):
            raise FileNotFoundError(f"OVMF code not found: {self.cfg.ovmf_code}")
        if self.cfg.ovmf_vars and not os.path.exists(self.cfg.ovmf_vars):
            raise FileNotFoundError(f"OVMF vars not found: {self.cfg.ovmf_vars}")
        if not os.path.isdir(self.cfg.esp_dir):
            raise FileNotFoundError(f"ESP dir not found: {self.cfg.esp_dir}")

        cmd = build_qemu_cmd(self.cfg)
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        self.buf = b""

    def close(self) -> None:
        if not self.proc:
            return
        try:
            if self.proc.poll() is None:
                try:
                    self.force_stop()
                    self.proc.terminate()
                    self.proc.wait(timeout=5)
                except Exception:
                    self.proc.kill()
        finally:
            if self.selector:
                try:
                    if self.proc and self.proc.stdout:
                        try:
                            self.selector.unregister(self.proc.stdout)
                        except Exception:
                            pass
                    self.selector.close()
                finally:
                    self.selector = None

            if self.proc:
                if self.proc.stdin:
                    try:
                        self.proc.stdin.close()
                    except Exception:
                        pass
                if self.proc.stdout:
                    try:
                        self.proc.stdout.close()
                    except Exception:
                        pass
            self.proc = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    def send(self, line: str) -> None:
        if not self.proc or not self.proc.stdin:
            raise RuntimeError("QEMU process not running")
        self.proc.stdin.write(line.encode("utf-8") + b"\r\n")
        self.proc.stdin.flush()

    def force_stop(self) -> None:
        if not self.proc or not self.proc.stdin:
            return
        if self.proc.poll() is not None:
            return
        try:
            self.proc.stdin.write(b"\x01x")
            self.proc.stdin.flush()
        except Exception:
            return

    def _pump(self, timeout_s: float) -> None:
        if not self.selector or not self.proc:
            return
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return
            events = self.selector.select(timeout=0.1)
            for key, _ in events:
                chunk = key.fileobj.read(4096)
                if not chunk:
                    continue
                if self.echo:
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
                self.buf += chunk

    def expect(self, needle: Union[bytes, str, Pattern[bytes]], timeout_s: Optional[int] = None) -> bool:
        if isinstance(needle, str):
            needle_b = needle.encode("utf-8")
            pattern = None
        elif isinstance(needle, bytes):
            needle_b = needle
            pattern = None
        else:
            needle_b = b""
            pattern = needle

        limit = timeout_s if timeout_s is not None else self.timeout_s
        deadline = time.time() + limit
        while time.time() < deadline:
            if pattern:
                if pattern.search(self.buf):
                    return True
            else:
                if needle_b in self.buf:
                    return True
            self._pump(0.2)
        if self.force_stop_on_timeout:
            self.force_stop()
        return False

    def mark(self) -> int:
        return len(self.buf)

    def expect_from(self, start: int, needle: Union[bytes, str, Pattern[bytes]], timeout_s: Optional[int] = None) -> bool:
        if isinstance(needle, str):
            needle_b = needle.encode("utf-8")
            pattern = None
        elif isinstance(needle, bytes):
            needle_b = needle
            pattern = None
        else:
            needle_b = b""
            pattern = needle

        limit = timeout_s if timeout_s is not None else self.timeout_s
        deadline = time.time() + limit
        while time.time() < deadline:
            if start < 0:
                start = 0
            view = self.buf[start:]
            if pattern:
                if pattern.search(view):
                    return True
            else:
                if needle_b in view:
                    return True
            self._pump(0.2)
        if self.force_stop_on_timeout:
            self.force_stop()
        return False

    def tail(self, max_bytes: int = 2048) -> str:
        if max_bytes <= 0:
            return ""
        data = self.buf[-max_bytes:]
        return data.decode("utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser(description="QEMU test framework smoke run.")
    parser.add_argument("--ovmf-code", default="")
    parser.add_argument("--ovmf-vars", default="")
    parser.add_argument("--esp", default="")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    if args.ovmf_code or args.esp:
        cfg = QemuConfig(args.ovmf_code, args.ovmf_vars, args.esp)
    else:
        cfg = default_config()

    with QemuSession(cfg, timeout_s=args.timeout) as session:
        if not session.expect(b"wamos> "):
            return 1
        session.send("halt")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
