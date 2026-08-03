#!/usr/bin/env python3
"""Generate the WASM host-call ABI from abi/hostcalls.yaml.

Generates (into abi/generated/c/):
  - wasmos_hostcall_ids.h    the kernel HC_* id enum
  - wasmos_symbols_warp.inc  the WARP WASMOS_SYMBOLS(LINK) table
  - wasmos_link_wasm3.inc    the wasm3 link table (wasm3_link_raw calls + m3 sigs)

and verifies them against the live hand-written sources
(src/kernel/include/warp_ring3.h, src/kernel/warp/link.cpp,
src/kernel/wasm3/link.c). These are parallel artifacts used to prove the IDL is
faithful before the kernel is rewired; wrapper *bodies* stay hand-written.

See docs/architecture/34-abi-idl-and-error-model.md.

ID guarantees enforced by the Model: ids unique, dense 0..N-1, ordered
(retired slots must be explicit `reserved: true`), HC symbols unique.

Usage:
    gen_abi_hostcalls.py [--check] [--verify-source]
      (no args)        regenerate all generated host-call files
      --check          fail (exit 2) if any on-disk file differs from the IDL
      --verify-source  fail (exit 3) if the IDL does not reproduce the live
                       HC_* enum, the WARP WASMOS_SYMBOLS table, and the wasm3
                       link table (ids, symbols, sigs, wrapper fn names)
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDL_PATH = os.path.join(REPO_ROOT, "abi", "hostcalls.yaml")
GEN_DIR = os.path.join(REPO_ROOT, "abi", "generated", "c")
IDS_OUT = os.path.join(GEN_DIR, "wasmos_hostcall_ids.h")
WARP_OUT = os.path.join(GEN_DIR, "wasmos_symbols_warp.inc")
WASM3_OUT = os.path.join(GEN_DIR, "wasmos_link_wasm3.inc")

WARP_RING3_H = os.path.join(REPO_ROOT, "src", "kernel", "include", "warp_ring3.h")
WARP_LINK_CPP = os.path.join(REPO_ROOT, "src", "kernel", "warp", "link.cpp")
WASM3_LINK_C = os.path.join(REPO_ROOT, "src", "kernel", "wasm3", "link.c")

# HC_* symbol prefix and warp_/wasmos_ fn prefix, keyed by module.
SYM_PREFIX = {"wasmos": "", "env": "ENV_", "wasi_snapshot_preview1": "WASI_"}
FN_PREFIX = {"wasmos": "", "env": "env_", "wasi_snapshot_preview1": "wasi_"}


def die(msg, code=1):
    print(f"gen_abi_hostcalls: {msg}", file=sys.stderr)
    sys.exit(code)


class YamlUnavailable(RuntimeError):
    pass


def load_idl():
    try:
        import yaml
    except ImportError as exc:
        raise YamlUnavailable(
            "PyYAML is required to regenerate the host-call ABI (pip install pyyaml); "
            "the checked-in generated files are used for normal builds."
        ) from exc
    with open(IDL_PATH, "r", encoding="utf-8") as f:
        return yaml.safe_load(f)


def hc_symbol(entry):
    module = entry.get("module", "wasmos")
    if module not in SYM_PREFIX:
        die(f"unknown module '{module}' for host call '{entry.get('name')}'")
    if entry.get("reserved"):
        return f"HC_RESERVED_{entry['id']}"
    return "HC_" + SYM_PREFIX[module] + entry["name"].upper()


class Model:
    def __init__(self, idl):
        self.version = int(idl["abi_version"])
        self.hostcalls = idl["hostcalls"]
        self.extra = idl.get("extra", []) or []

        by_id = {}
        for e in self.hostcalls:
            if "id" not in e:
                die(f"host call '{e.get('name')}' has no id")
            i = int(e["id"])
            if i in by_id:
                die(f"duplicate id {i}: '{by_id[i].get('name')}' and '{e.get('name')}'")
            by_id[i] = e
        n = len(self.hostcalls)
        missing = [i for i in range(n) if i not in by_id]
        if missing:
            die(
                f"id space has gaps (not dense 0..{n - 1}): missing {missing}. "
                f"Retired slots must be explicit `reserved: true` entries."
            )
        self.ordered = [by_id[i] for i in range(n)]
        self.count = n

        syms = {}
        for e in self.ordered:
            s = hc_symbol(e)
            if s in syms:
                die(f"duplicate HC symbol {s} (ids {syms[s]} and {e['id']})")
            syms[s] = e["id"]
        self.symbols = syms

        # alias resolution is by name within the wasmos module
        self.by_name = {
            e["name"]: e
            for e in self.hostcalls
            if e.get("module", "wasmos") == "wasmos"
        }

    # --- derivations --------------------------------------------------------
    def _target(self, e):
        return self.by_name[e["alias_of"]] if "alias_of" in e else e

    def module(self, e):
        return e.get("module", "wasmos")

    def runtimes(self, e):
        return e.get("runtimes", ["warp", "wasm3"])

    def warp_fn(self, e):
        if "alias_of" in e:
            return "warp_" + FN_PREFIX["wasmos"] + e["alias_of"]
        return "warp_" + FN_PREFIX[self.module(e)] + e["name"]

    def wasm3_fn(self, e):
        if "alias_of" in e:
            return self.wasm3_fn(self.by_name[e["alias_of"]])
        if "wasm3_fn" in e:
            return e["wasm3_fn"]
        return "wasmos_" + e["name"]

    def wasm3_sig(self, e):
        t = self._target(e)
        ret = "v" if t.get("returns") == "void" else "i"
        chars = "".join(self._sig_char(p) for p in (t.get("params") or []))
        return f"{ret}({chars})"

    @staticmethod
    def _sig_char(p):
        if p.get("wasm3") == "i32":
            return "i"
        return "*" if p["kind"] in ("ptr", "buf", "out") else "i"

    def alias_comment(self, e):
        if "alias_of" in e:
            return f" /* alias: {e['name']} → warp_{e['alias_of']} */"
        if e.get("reserved"):
            return " /* reserved (retired host call; slot kept for id stability) */"
        return ""


BANNER = [
    "/*",
    " * GENERATED by scripts/gen_abi_hostcalls.py from abi/hostcalls.yaml — DO NOT EDIT.",
    " * See docs/architecture/34-abi-idl-and-error-model.md.",
    " */",
]


def emit_hostcall_ids(m):
    o = list(BANNER)
    w = o.append
    w("#ifndef WASMOS_HOSTCALL_IDS_H")
    w("#define WASMOS_HOSTCALL_IDS_H")
    w("")
    w("/* Ordered hostcall IDs — must match WASMOS_SYMBOLS expansion order. */")
    w("typedef enum {")
    for e in m.ordered:
        w(f"    {hc_symbol(e)} = {e['id']},{m.alias_comment(e)}")
    w(f"    HC_COUNT = {m.count},")
    w("} warp_hostcall_id_t;")
    w("")
    w("#endif /* WASMOS_HOSTCALL_IDS_H */")
    w("")
    return "\n".join(o)


def emit_warp_symbols(m):
    """WARP WASMOS_SYMBOLS(LINK) — every host call in id order (position == id)."""
    o = list(BANNER)
    w = o.append
    w("/* WARP native-symbol table; expanded 3 ways (STATIC/JIT, DYNAMIC/AOT,")
    w(" * ring-3). Order is id order — table position IS the ring-3 hostcall id. */")
    w("#define WASMOS_SYMBOLS(LINK) \\")
    lines = []
    for e in m.ordered:
        mod = m.module(e)
        lines.append(f'    LINK("{mod}", "{e["name"]}", {m.warp_fn(e)})')
    w(" , \\\n".join(lines))
    w("")
    return "\n".join(o)


def emit_wasm3_links(m):
    """wasm3 link table for the "wasmos" module (env/wasi live in separate tables)."""
    o = list(BANNER)
    w = o.append
    w('/* wasm3 link calls for module "wasmos" (m3 sig strings derived from the')
    w(" * IDL param kinds; `*` = m3-translated guest pointer, `i` = raw i32). */")
    for e in m.ordered:
        if m.module(e) != "wasmos" or "wasm3" not in m.runtimes(e):
            continue
        w(
            f'    rc |= wasm3_link_raw(module, "wasmos", "{e["name"]}", '
            f'"{m.wasm3_sig(e)}", {m.wasm3_fn(e)});'
        )
    w("")
    return "\n".join(o)


OUTPUTS = [
    (IDS_OUT, emit_hostcall_ids),
    (WARP_OUT, emit_warp_symbols),
    (WASM3_OUT, emit_wasm3_links),
]


# --------------------------------------------------------------------------- verify


def _read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def verify_enum(m):
    text = _read(WARP_RING3_H)
    src = {s: int(v) for s, v in re.findall(r"\b(HC_[A-Z0-9_]+)\s*=\s*(\d+)", text)}
    src_count = src.pop("HC_COUNT", None)
    problems = []
    if set(src) != set(m.symbols):
        problems.append(
            f"enum symbol set differs: only-src={sorted(set(src) - set(m.symbols))} "
            f"only-idl={sorted(set(m.symbols) - set(src))}"
        )
    for s in set(src) & set(m.symbols):
        if src[s] != m.symbols[s]:
            problems.append(f"enum id mismatch {s}: src={src[s]} idl={m.symbols[s]}")
    if src_count != m.count:
        problems.append(f"HC_COUNT mismatch: src={src_count} idl={m.count}")
    return problems


def verify_warp(m):
    text = _read(WARP_LINK_CPP)
    region = text.split("#define WASMOS_SYMBOLS(LINK)", 1)[1]
    region = region.split("warp_wasmos_symbols", 1)[0]
    # The macro uses C block comments and `\` line-continuations, some splitting
    # a LINK entry mid-arguments; strip both so entries parse regardless of wrap.
    region = re.sub(r"/\*.*?\*/", " ", region, flags=re.S)
    region = region.replace("\\", " ")
    src = re.findall(r'LINK\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*\)', region)
    gen = [(m.module(e), e["name"], m.warp_fn(e)) for e in m.ordered]
    problems = []
    if len(src) != len(gen):
        problems.append(f"WARP table length differs: src={len(src)} idl={len(gen)}")
    for i, (s, g) in enumerate(zip(src, gen)):
        if s != g:
            problems.append(f"WARP entry {i} differs: src={s} idl={g}")
    return problems


def verify_wasm3(m):
    text = _read(WASM3_LINK_C)
    src = {
        name: (sig, fn)
        for name, sig, fn in re.findall(
            r'wasm3_link_raw\(\s*module\s*,\s*"wasmos"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*\)',
            text,
        )
    }
    gen = {
        e["name"]: (m.wasm3_sig(e), m.wasm3_fn(e))
        for e in m.ordered
        if m.module(e) == "wasmos" and "wasm3" in m.runtimes(e)
    }
    problems = []
    if set(src) != set(gen):
        problems.append(
            f"wasm3 name set differs: only-src={sorted(set(src) - set(gen))} "
            f"only-idl={sorted(set(gen) - set(src))}"
        )
    for name in set(src) & set(gen):
        if src[name] != gen[name]:
            problems.append(f"wasm3 {name} differs: src={src[name]} idl={gen[name]}")
    return problems


def verify_source(m):
    problems = verify_enum(m) + verify_warp(m) + verify_wasm3(m)
    if problems:
        for p in problems:
            print(f"gen_abi_hostcalls: {p}", file=sys.stderr)
        die("IDL does not match the live sources — reconcile before generating", code=3)
    print(
        f"gen_abi_hostcalls: IDL matches warp_ring3.h + WASMOS_SYMBOLS + wasm3 link table "
        f"({m.count} host calls, ids 0..{m.count - 1})"
    )


def main():
    ap = argparse.ArgumentParser(description="Generate the WASM host-call ABI.")
    ap.add_argument(
        "--check",
        action="store_true",
        help="fail if any on-disk generated file differs from the IDL",
    )
    ap.add_argument(
        "--verify-source",
        action="store_true",
        help="fail if the IDL does not reproduce the live kernel tables",
    )
    args = ap.parse_args()

    try:
        model = Model(load_idl())
    except YamlUnavailable as exc:
        if args.check or args.verify_source:
            print(f"gen_abi_hostcalls: skipping ({exc})", file=sys.stderr)
            return
        die(str(exc))

    if args.verify_source:
        verify_source(model)
        return

    if args.check:
        stale = False
        for path, emit in OUTPUTS:
            want = emit(model)
            have = _read(path) if os.path.exists(path) else None
            if have != want:
                print(
                    f"gen_abi_hostcalls: {os.path.relpath(path, REPO_ROOT)} is stale",
                    file=sys.stderr,
                )
                stale = True
        if stale:
            die("run scripts/gen_abi_hostcalls.py to regenerate", code=2)
        print("gen_abi_hostcalls: all generated host-call files are up to date")
        return

    os.makedirs(GEN_DIR, exist_ok=True)
    for path, emit in OUTPUTS:
        with open(path, "w", encoding="utf-8") as f:
            f.write(emit(model))
        print(f"gen_abi_hostcalls: wrote {os.path.relpath(path, REPO_ROOT)}")


if __name__ == "__main__":
    main()
