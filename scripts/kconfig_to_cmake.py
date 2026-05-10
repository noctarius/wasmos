#!/usr/bin/env python3
"""Translate a Kconfig .config file into a CMake cache include."""

from __future__ import annotations

import argparse
from pathlib import Path


BOOL_KEYS = {
    "AS_ENABLE",
    "RUST_ENABLE",
    "GO_ENABLE",
    "ZIG_ENABLE",
    "WASMOS_TRACE",
    "WASMOS_RING3_SMOKE",
    "WASMOS_RING3_THREAD_LIFECYCLE_SMOKE",
}

STRING_KEYS = {"KERNEL_TARGET_TRIPLE"}
INT_KEYS = {"QEMU_GDB_PORT"}
INITIALIZED_GUARDS = {"AS_ENABLE", "RUST_ENABLE", "GO_ENABLE", "ZIG_ENABLE"}
# TODO: Extend this symbol map as additional CMake cache settings migrate to Kconfig.


def parse_config_line(raw: str) -> tuple[str, str] | None:
    line = raw.strip()
    if not line or line.startswith("#"):
        return None
    if not line.startswith("CONFIG_") or "=" not in line:
        return None
    key, value = line.split("=", 1)
    return key.removeprefix("CONFIG_"), value.strip()


def cmake_bool(value: str) -> str:
    return "ON" if value.lower() in {"y", "yes", "1", "on", "true"} else "OFF"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="Path to Kconfig .config file")
    parser.add_argument("--output", required=True, help="Path to generated CMake include")
    args = parser.parse_args()

    in_path = Path(args.input)
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    values: dict[str, str] = {}
    if in_path.exists():
        for raw in in_path.read_text(encoding="utf-8").splitlines():
            parsed = parse_config_line(raw)
            if parsed is None:
                continue
            key, value = parsed
            values[key] = value

    lines: list[str] = []
    lines.append("# Auto-generated from Kconfig .config")
    lines.append(f"# Source: {in_path}")
    for key in sorted(values.keys()):
        value = values[key]
        if key in BOOL_KEYS:
            lines.append(f"set({key} {cmake_bool(value)} CACHE BOOL \"Kconfig imported\" FORCE)")
        elif key in STRING_KEYS:
            lines.append(f"set({key} {value} CACHE STRING \"Kconfig imported\" FORCE)")
        elif key in INT_KEYS:
            lines.append(f"set({key} {value} CACHE STRING \"Kconfig imported\" FORCE)")

    for key in sorted(INITIALIZED_GUARDS):
        if key in values:
            lines.append(f"set({key}_INITIALIZED ON CACHE BOOL \"Kconfig imported\" FORCE)")

    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
