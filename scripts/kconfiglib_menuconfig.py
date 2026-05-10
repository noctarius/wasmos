#!/usr/bin/env python3
"""Minimal interactive Kconfig editor powered by kconfiglib."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def _require_kconfiglib():
    try:
        import kconfiglib  # type: ignore
    except ModuleNotFoundError:
        print(
            "kconfiglib is not installed in the active Python environment.\n"
            "Install with: python3 -m pip install kconfiglib",
            file=sys.stderr,
        )
        raise SystemExit(2)
    return kconfiglib


def _bool_display(value: str) -> str:
    return "y" if value == "y" else "n"


def _editable_symbols(kconf):
    symbols = []
    for sym in kconf.unique_defined_syms:
        if not sym.name or not sym.nodes:
            continue
        node = sym.nodes[0]
        prompt = node.prompt[0] if node.prompt else None
        if not prompt:
            continue
        if sym.visibility <= 0:
            continue
        if sym.type in (kconf.BOOL, kconf.STRING, kconf.INT):
            symbols.append((sym, prompt))
    return symbols


def _set_symbol_value(sym, new_value: str):
    if sym.type == sym.kconfig.BOOL:
        val = new_value.strip().lower()
        if val in {"y", "yes", "1", "on", "true"}:
            sym.set_value("y")
            return
        if val in {"n", "no", "0", "off", "false"}:
            sym.set_value("n")
            return
        raise ValueError("bool symbols accept y/n")
    if sym.type == sym.kconfig.INT:
        int(new_value, 10)
        sym.set_value(new_value)
        return
    sym.set_value(new_value)


def _run_interactive(kconf, config_path: Path):
    while True:
        symbols = _editable_symbols(kconf)
        print("\n=== WASMOS Kconfig (kconfiglib) ===")
        print(f"Config: {config_path}")
        for idx, (sym, prompt) in enumerate(symbols, start=1):
            if sym.type == kconf.BOOL:
                val = _bool_display(sym.str_value)
            else:
                val = sym.str_value
            print(f"{idx:2d}. {sym.name} = {val}    ({prompt})")
        print("\nCommands: <number> edit, w write, q quit")
        choice = input("> ").strip().lower()
        if choice == "q":
            return
        if choice == "w":
            kconf.write_config(str(config_path))
            print(f"wrote {config_path}")
            return
        if not choice.isdigit():
            print("invalid selection")
            continue
        idx = int(choice)
        if idx < 1 or idx > len(symbols):
            print("selection out of range")
            continue
        sym, prompt = symbols[idx - 1]
        current = sym.str_value
        raw = input(f"{sym.name} ({prompt}) [{current}] -> ").strip()
        if not raw:
            continue
        try:
            _set_symbol_value(sym, raw)
        except ValueError as exc:
            print(f"invalid value: {exc}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kconfig", required=True, help="Path to top-level Kconfig")
    parser.add_argument("--config", required=True, help="Path to .config")
    parser.add_argument("--defconfig", required=True, help="Path to defconfig seed")
    args = parser.parse_args()

    kconfiglib = _require_kconfiglib()
    config_path = Path(args.config)
    defconfig_path = Path(args.defconfig)
    config_path.parent.mkdir(parents=True, exist_ok=True)

    kconf = kconfiglib.Kconfig(args.kconfig, warn=False)
    if config_path.exists():
        kconf.load_config(str(config_path))
    elif defconfig_path.exists():
        kconf.load_config(str(defconfig_path))
    else:
        kconf.write_config(str(config_path))
        kconf.load_config(str(config_path))

    _run_interactive(kconf, config_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
