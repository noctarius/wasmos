#!/usr/bin/env python3
"""Resolve WASMOS kernel panic addresses to function and source locations."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


PANIC_ADDR_RE = re.compile(r"\b(?P<kind>rip|ret)=(?P<addr>[0-9a-fA-F]{16})\b")
CPU_HEADER_RE = re.compile(r"^--- CPU (?P<cpu>\d+)\b")
FRAME_RE = re.compile(r"^\s*\[(?P<frame>\d+)\]")


@dataclass(frozen=True)
class PanicAddress:
    cpu: int | None
    kind: str
    frame: int | None
    addr_hex: str


def parse_panic_addresses(text: str) -> list[PanicAddress]:
    entries: list[PanicAddress] = []
    current_cpu: int | None = None
    current_frame: int | None = None

    for line in text.splitlines():
        cpu_match = CPU_HEADER_RE.match(line)
        if cpu_match:
            current_cpu = int(cpu_match.group("cpu"))
            current_frame = None
            continue

        frame_match = FRAME_RE.match(line)
        current_frame = int(frame_match.group("frame")) if frame_match else None

        for match in PANIC_ADDR_RE.finditer(line):
            entries.append(
                PanicAddress(
                    cpu=current_cpu,
                    kind=match.group("kind"),
                    frame=current_frame if match.group("kind") == "ret" else None,
                    addr_hex=match.group("addr").lower(),
                )
            )

    return entries


def unique_addresses(entries: Iterable[PanicAddress]) -> list[str]:
    seen: set[str] = set()
    ordered: list[str] = []
    for entry in entries:
        if entry.addr_hex in seen:
            continue
        seen.add(entry.addr_hex)
        ordered.append(entry.addr_hex)
    return ordered


def resolve_addresses(addr2line: str, kernel: str, addresses: list[str]) -> dict[str, str]:
    if not addresses:
        return {}

    proc = subprocess.run(
        [addr2line, "-e", kernel, "-f", "-C", "-p", *("0x" + addr for addr in addresses)],
        check=True,
        capture_output=True,
        text=True,
    )
    lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    if len(lines) != len(addresses):
        raise RuntimeError(
            f"addr2line returned {len(lines)} lines for {len(addresses)} addresses"
        )
    return dict(zip(addresses, lines))


def render(entries: list[PanicAddress], resolved: dict[str, str]) -> str:
    lines = []
    for entry in entries:
        prefix = f"cpu{entry.cpu}" if entry.cpu is not None else "cpu?"
        if entry.kind == "rip":
            label = "rip"
        else:
            frame = "?" if entry.frame is None else str(entry.frame)
            label = f"frame[{frame}]"
        decoded = resolved.get(entry.addr_hex, "(unresolved)")
        lines.append(f"{prefix} {label} 0x{entry.addr_hex} -> {decoded}")
    return "\n".join(lines)


def read_input(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def default_kernel_path() -> str:
    return os.path.join("build", "kernel.elf")


def read_cmake_cache(cache_path: Path) -> dict[str, str]:
    data: dict[str, str] = {}
    if not cache_path.exists():
        return data

    for raw_line in cache_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("//") or line.startswith("#"):
            continue
        if ":" not in line or "=" not in line:
            continue
        key, rest = line.split(":", 1)
        _kind, value = rest.split("=", 1)
        data[key] = value
    return data


def discover_addr2line(requested: str, kernel: str) -> str | None:
    if requested:
        if os.path.isabs(requested) or os.path.dirname(requested):
            return requested if os.path.exists(requested) else None
        found = shutil.which(requested)
        if found:
            return found

    kernel_path = Path(kernel).resolve()
    build_dir = kernel_path.parent
    cache = read_cmake_cache(build_dir / "CMakeCache.txt")
    clang_path = cache.get("CLANG", "")

    candidates: list[Path] = []
    if clang_path:
        candidates.append(Path(clang_path).resolve().parent / "llvm-addr2line")
        candidates.append(Path(clang_path).resolve().parent / "addr2line")
    candidates.extend(
        [
            Path("/opt/homebrew/opt/llvm/bin/llvm-addr2line"),
            Path("/usr/local/opt/llvm/bin/llvm-addr2line"),
            Path("/opt/homebrew/bin/llvm-addr2line"),
            Path("/usr/local/bin/llvm-addr2line"),
        ]
    )

    for candidate in candidates:
        if candidate.exists():
            return str(candidate)

    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Resolve WASMOS kernel panic RIP/backtrace addresses to file:line."
    )
    parser.add_argument(
        "input",
        nargs="?",
        default="-",
        help="panic log file to decode, or - to read from stdin",
    )
    parser.add_argument(
        "--kernel",
        default=default_kernel_path(),
        help="path to the kernel ELF with debug info (default: build/kernel.elf)",
    )
    parser.add_argument(
        "--addr2line",
        default=os.environ.get("LLVM_ADDR2LINE", ""),
        help="addr2line-compatible tool to use (default: auto-discover from CMakeCache/LLVM install)",
    )
    args = parser.parse_args()

    if not os.path.exists(args.kernel):
        print(f"kernel ELF not found: {args.kernel}", file=sys.stderr)
        return 1

    text = read_input(args.input)
    entries = parse_panic_addresses(text)
    if not entries:
        print("no panic addresses found", file=sys.stderr)
        return 1

    addr2line = discover_addr2line(args.addr2line, args.kernel)
    if not addr2line:
        print(
            "failed to locate llvm-addr2line; pass --addr2line or set LLVM_ADDR2LINE",
            file=sys.stderr,
        )
        return 1

    try:
        resolved = resolve_addresses(addr2line, args.kernel, unique_addresses(entries))
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        print(f"failed to resolve addresses: {exc}", file=sys.stderr)
        return 1

    print(render(entries, resolved))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
