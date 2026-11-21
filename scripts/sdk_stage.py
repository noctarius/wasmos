#!/usr/bin/env python3
"""Populate a staged WASMOS SDK's sysroot headers, wrappers, libexec and share/.

The archives (libc.a, libsys.a, crt1.o) are built by the build system into
<sdk>/sysroot/lib/<triple>; this script stages everything else and writes
<sdk>/wasmos-sdk.conf, which the wrappers source.

Header rewrite: several public headers include the generated ABI headers by a
repo-relative path (../../../../abi/generated/c/...), at more than one depth.
No sysroot can reproduce that layout, so every staged header has those includes
rewritten to <wasmos/abi/...>, and the rewrite is asserted: a silent miss would
produce a sysroot that cannot compile anything that talks to a service.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import stat
import sys
from pathlib import Path

ABI_INCLUDE = re.compile(r"(\.\./)+abi/generated/c/")
WRAPPERS = [
    "wasmos-clang",
    "wasmos-clang++",
    "wasmos-zig",
    "wasmos-asc",
    "wasmos-rustc",
    "wasmos-tinygo",
    "wasmos-pack",
    "wasmos-inspect",
]
EXEC_MODE = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR | stat.S_IRGRP | stat.S_IXGRP
CONF_KEYS = [
    "VERSION",
    "TARGET",
    "TARGET_LLVM",
    "CLANG",
    "CLANGXX",
    "ZIG",
    "ZIG_STACK_SIZE",
    "ZIG_VA_LIMIT",
    "ASC",
    "RUSTC",
    "TINYGO",
    "WASMOPT",
]


def copy(src: Path, dst_dir: Path, mode: int | None = None, overwrite: bool = True):
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / src.name
    if not overwrite and dst.exists():
        return
    shutil.copyfile(src, dst)
    if mode is not None:
        os.chmod(dst, mode)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, required=True)
    ap.add_argument("--sdk", type=Path, required=True)
    ap.add_argument("--packer", type=Path)
    ap.add_argument("--as-source", type=Path, action="append", default=[])
    ap.add_argument(
        "--tool",
        action="append",
        default=[],
        metavar="NAME=PATH",
        help="binutils face: wasmos-NAME execs PATH",
    )
    ap.add_argument("--conf", action="append", default=[], metavar="KEY=VALUE")
    ap.add_argument("--stamp", type=Path, required=True)
    args = ap.parse_args()

    src, sdk = args.src, args.sdk
    libc = src / "src/libc"
    libsys = src / "src/libsys/wasm"
    drivers = src / "src/drivers/include"
    inc = sdk / "sysroot/include"

    # --- headers -------------------------------------------------------------
    for h in libc.glob("include/*.h"):
        copy(h, inc)
    for h in libc.glob("include/sys/*.h"):
        copy(h, inc / "sys")
    # Staged in the in-tree include-search order (libui, libc, libsys), first
    # copy wins, so a sysroot build resolves <wasmos/x.h> to the same file an
    # in-tree build does.
    # TODO: libc and libsys each carry their own diverged wasmos/mutex.h and
    # wasmos/rtc_ipc.h; the sysroot ships libc's and drops libsys's copy, so an
    # SDK app and a libsys object can see different declarations of one header.
    wasmos_inc = inc / "wasmos"
    shutil.rmtree(wasmos_inc, ignore_errors=True)
    for d in (
        src / "src/libui/include/wasmos",
        libc / "include/wasmos",
        libsys / "include/wasmos",
    ):
        for h in sorted(d.glob("*.h")):
            copy(h, wasmos_inc, overwrite=False)
    # src/drivers/include holds the headers wasmos/{ipc,proc,net}.h and libsys.h
    # include by bare name; they are part of the public surface.
    for h in drivers.glob("*.h"):
        copy(h, inc)
    for h in drivers.glob("wasmos/*.h"):
        copy(h, inc / "wasmos")
    for h in (src / "abi/generated/c").glob("*.h"):
        copy(h, inc / "wasmos/abi")

    for h in inc.rglob("*.h"):
        text = h.read_text()
        rewritten = ABI_INCLUDE.sub("wasmos/abi/", text)
        if rewritten != text:
            h.write_text(rewritten)
    for h in inc.rglob("*.h"):
        if ABI_INCLUDE.search(h.read_text()):
            sys.exit(f"sdk_stage: {h} still carries a repo-relative ABI include")
    if not (inc / "wasmos/abi/wasmos_imports.h").exists():
        sys.exit("sdk_stage: generated ABI headers missing from the sysroot")

    # --- host tools the wrappers front ---------------------------------------
    libexec = sdk / "libexec/wasmos"
    if args.packer and args.packer.exists():
        copy(args.packer, libexec, EXEC_MODE)
    copy(src / "scripts/wasm_inspect.py", libexec)
    copy(src / "scripts/wasm_stack_check.py", libexec)

    # --- driver + tool wrappers ----------------------------------------------
    for w in WRAPPERS:
        copy(src / "scripts/sdk" / w, sdk / "bin", EXEC_MODE)
    for spec in args.tool:
        name, _, tool = spec.partition("=")
        if not tool or not Path(tool).exists():
            continue
        path = sdk / "bin" / f"wasmos-{name}"
        path.write_text(
            f"#!/bin/sh\n# wasmos-{name} - the SDK's {tool} face. "
            f'Generated by scripts/sdk_stage.py.\nexec "{tool}" "$@"\n'
        )
        os.chmod(path, EXEC_MODE)

    # --- share/ --------------------------------------------------------------
    share = sdk / "share/wasmos"
    for z in ("wasmos.zig", "coroutine.zig"):
        copy(libc / "zig" / z, share / "zig")
    for h in libc.glob("zig/compat/*.h"):
        copy(h, share / "zig/compat")
    copy(libc / "assemblyscript/runtime.ts", share / "assemblyscript")
    for a in args.as_source:
        copy(a, share / "assemblyscript")
    if (src / "tools/as_coroutine_transform.mjs").exists():
        copy(src / "tools/as_coroutine_transform.mjs", libexec)
    for r in ("wasmos.rs", "coroutine.rs"):
        copy(libc / "rust" / r, share / "rust")
    for g in ("wasmos.go", "coroutine.go"):
        copy(libc / "go" / g, share / "go")
    for c in (
        "coroutine_wasm.c",
        "ipc_future_wasm.c",
        "go_coroutine_trampoline.c",
        "service_runtime_wasm.c",
        "go_async_app_wasm.c",
    ):
        copy(libsys / c, share / "go/c")
    copy(src / "scripts/sdk/default-manifest.toml", share)
    copy(src / "scripts/sdk/WASMOSToolchain.cmake", sdk / "share/cmake/WASMOS")
    copy(src / "scripts/sdk/Platform/WASMOS.cmake", sdk / "share/cmake/WASMOS/Platform")

    # --- wasmos-sdk.conf -----------------------------------------------------
    conf = dict(c.partition("=")[::2] for c in args.conf)
    lines = ["# Generated by scripts/sdk_stage.py - do not edit."]
    lines += [f"WASMOS_SDK_{k}={conf.get(k, '')}" for k in CONF_KEYS]
    (sdk / "wasmos-sdk.conf").write_text("\n".join(lines) + "\n")

    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text("staged\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
