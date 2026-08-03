#!/usr/bin/env python3
"""Generate the WASM host-call ABI from abi/hostcalls.yaml.

Generates (into abi/generated/c/):
  - wasmos_hostcall_ids.h    the kernel HC_* id enum
  - wasmos_symbols_warp.inc  the WARP WASMOS_SYMBOLS(LINK) table
  - wasmos_link_wasm3.inc    the wasm3 link X-macro WASMOS_WASM3_LINKS(X) (m3 sigs)
  - wasmos_symbols_aot.inc   the WARP AOT symbol table WASMOS_AOT_SYMBOLS(LINK)
  - wasmos_ring3_dispatch.inc  warp_ring3_dispatch_table() (arg-unpack switch)

and verifies them against the live hand-written sources
(src/kernel/include/warp_ring3.h, src/kernel/warp/link.cpp,
src/kernel/wasm3/link.c, src/tools/warp_aot/warp_aot.cpp). These are parallel
artifacts used to prove the IDL is faithful before the kernel is rewired;
wrapper *bodies* stay hand-written.

See docs/architecture/34-abi-idl-and-error-model.md.

ID guarantees enforced by the Model: ids unique, dense 0..N-1, ordered
(retired slots must be explicit `reserved: true`), HC symbols unique.

Usage:
    gen_abi_hostcalls.py [--check] [--verify-source]
      (no args)        regenerate all generated host-call files
      --check          fail (exit 2) if any on-disk file differs from the IDL
      --verify-source  fail (exit 3) if the IDL does not reproduce the live
                       HC_* enum, the WARP WASMOS_SYMBOLS table, the wasm3 link
                       table, and the WARP AOT symbol table (ids, symbols, m3
                       sigs, wrapper fn names, arg-count stubs; the AOT table may
                       be a superset since it is name-resolved)
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
AOT_OUT = os.path.join(GEN_DIR, "wasmos_symbols_aot.inc")
RING3_OUT = os.path.join(GEN_DIR, "wasmos_ring3_dispatch.inc")

WARP_RING3_H = os.path.join(REPO_ROOT, "src", "kernel", "include", "warp_ring3.h")
WARP_LINK_CPP = os.path.join(REPO_ROOT, "src", "kernel", "warp", "link.cpp")
WASM3_LINK_C = os.path.join(REPO_ROOT, "src", "kernel", "wasm3", "link.c")
WARP_AOT_CPP = os.path.join(REPO_ROOT, "src", "tools", "warp_aot", "warp_aot.cpp")

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

    def arity(self, e):
        """Number of wasm u32 params (excludes the trailing ctx)."""
        return len(self._target(e).get("params") or [])

    def returns(self, e):
        return self._target(e).get("returns")

    def ring3_call(self, e):
        """C++ call expr for a ring-3 dispatch case. The wrapper takes N u32
        params + a trailing void* ctx; ring-3 supplies ctx as the (N+1)-th arg.
        SysV puts the first 6 args in a0..a5, the rest on the user stack.
        ctx is therefore a<N> (register) or stack_u64(N-6) — computed, so the
        5-param case gets a5, not the hand-written a4 (ctx5) bug."""
        fn = self.warp_fn(e)
        n = self.arity(e)
        if n == 0:
            return f"{fn}(reinterpret_cast<void*>(a0))"
        if n + 1 <= 6:  # N params + ctx all in registers a0..a<N>
            args = [f"(uint32_t)a{i}" for i in range(n)]
            args.append(f"reinterpret_cast<void*>(a{n})")
            return f"{fn}({', '.join(args)})"
        # N+1 > 6: first 6 args in a0..a5, the remaining params + ctx on the stack
        args = [f"(uint32_t)a{i}" for i in range(6)]
        args += [f"(uint32_t)stack_u64({j - 6})" for j in range(6, n)]
        args.append(f"reinterpret_cast<void*>(stack_u64({n - 6}))")
        return f"{fn}({', '.join(args)})"

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
    """wasm3 host-call links for module "wasmos" as an X-macro.

    Pure data — no `rc`/`module` in scope, no accumulation policy baked in
    (mirroring WARP's WASMOS_SYMBOLS(LINK)). The hand-written linker owns the
    mechanism, e.g.:
        int rc = 0;
        #define X(mod, name, sig, fn) rc |= wasm3_link_raw(module, mod, name, sig, fn);
        WASMOS_WASM3_LINKS(X)
        #undef X
        if (rc != 0) { ... }
    env/wasi links live in separate tables.
    """
    o = list(BANNER)
    w = o.append
    w('/* wasm3 host-call links for module "wasmos", as an X-macro so the linking')
    w(" * mechanism (rc accumulation, error check) stays in the hand-written caller:")
    w(
        " *   #define X(mod, name, sig, fn) rc |= wasm3_link_raw(module, mod, name, sig, fn);"
    )
    w(" *   WASMOS_WASM3_LINKS(X)  #undef X")
    w(
        " * m3 sig: `*` = m3-translated guest pointer, `i` = raw i32 (incl. wasm3:i32). */"
    )
    w("#define WASMOS_WASM3_LINKS(X) \\")
    lines = []
    for e in m.ordered:
        if m.module(e) != "wasmos" or "wasm3" not in m.runtimes(e):
            continue
        lines.append(
            f'    X("wasmos", "{e["name"]}", "{m.wasm3_sig(e)}", {m.wasm3_fn(e)})'
        )
    w(" \\\n".join(lines))
    w("")
    return "\n".join(o)


def emit_aot_symbols(m):
    """WARP AOT symbol table — mirrors WASMOS_SYMBOLS in id order, but binds each
    entry to an arity-sized stub (stub_i<N>, N = wasm param count) that
    initFromCompiledBinary() rebinds to the live kernel fn at load. Emitted as a
    macro; the AOT tool expands it into its NativeSymbol[] initializer."""
    o = list(BANNER)
    w = o.append
    w("/* WARP AOT symbol table (name-resolved at load; kept in id order for")
    w(" * readability). Each entry binds a stub_i<N> of matching arity (N = wasm")
    w(" * param count, ctx excluded); initFromCompiledBinary() rebinds stubs to the")
    w(" * live kernel fns by name. The full host-call set is listed — an available")
    w(" * symbol no AOT module imports is harmless. Expand:")
    w(" *   static vb::NativeSymbol syms[] = { WASMOS_AOT_SYMBOLS(DYNAMIC_LINK) }; */")
    w("#define WASMOS_AOT_SYMBOLS(LINK) \\")
    lines = [
        f'    LINK("{m.module(e)}", "{e["name"]}", stub_i{m.arity(e)})'
        for e in m.ordered
    ]
    w(" , \\\n".join(lines))
    w("")
    return "\n".join(o)


def emit_ring3_dispatch(m):
    """Ring-3 hostcall dispatch as a self-contained inline function.

    warp_ring3_dispatch decodes the syscall frame into a0..a5 + user_rsp and
    calls this; the dispatch logic (the arg-unpacking switch) is generated, the
    frame decode stays hand-written. Needs only the HC_* enum and the warp_*
    wrapper declarations in scope. ctx is the computed (arity+1)-th arg (a<N> for
    register calls), so the 5-param case gets a5 — fixing the hand-written
    proc_info_stats ctx bug.
    """
    o = list(BANNER)
    w = o.append
    w("/* Generated ring-3 hostcall dispatch. Call from warp_ring3_dispatch after")
    w(" * decoding the syscall frame:")
    w(" *   return warp_ring3_dispatch_table(hc_id, a0,a1,a2,a3,a4,a5, user_rsp); */")
    w("static inline uint32_t warp_ring3_dispatch_table(uint32_t hc_id, uint64_t a0,")
    w("        uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,")
    w("        uint64_t user_rsp) {")
    w("    /* Stack args live past the return address at [user_rsp + 0]. */")
    w("    auto stack_u64 = [user_rsp](uint32_t n) -> uint64_t {")
    w("        return *reinterpret_cast<uint64_t*>(user_rsp + 8 + (uint64_t)n * 8);")
    w("    };")
    w("    (void)stack_u64;")
    w("    switch (hc_id) {")
    for e in m.ordered:
        w(f"    case {hc_symbol(e)}:")
        call = m.ring3_call(e)
        if m.returns(e) == "void":
            w(f"        {call};")
            w("        return 0;")
        else:
            w(f"        return {call};")
    w("    default:")
    w("        return (uint32_t)-1;")
    w("    }")
    w("}")
    w("")
    return "\n".join(o)


OUTPUTS = [
    (IDS_OUT, emit_hostcall_ids),
    (WARP_OUT, emit_warp_symbols),
    (WASM3_OUT, emit_wasm3_links),
    (AOT_OUT, emit_aot_symbols),
    (RING3_OUT, emit_ring3_dispatch),
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


def verify_aot(m):
    text = _read(WARP_AOT_CPP)
    region = text.split("aot_symbols", 1)[1]
    region = re.sub(r"/\*.*?\*/", " ", region, flags=re.S)
    region = region.replace("\\", " ")
    src = re.findall(
        r'DYNAMIC_LINK\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(stub_i\d+)\s*\)', region
    )
    # AOT is name-resolved, so the generated table may be a superset of the live
    # one: every current entry must be reproduced (name + arity), extras are OK.
    gen_stub = {(m.module(e), e["name"]): f"stub_i{m.arity(e)}" for e in m.ordered}
    problems = []
    for mod, name, stub in src:
        g = gen_stub.get((mod, name))
        if g is None:
            problems.append(f"AOT entry {mod}.{name} in warp_aot.cpp but not generated")
        elif g != stub:
            problems.append(f"AOT stub arity mismatch {mod}.{name}: src={stub} idl={g}")
    src_names = {(mod, name) for mod, name, _ in src}
    additions = [
        (m.module(e), e["name"])
        for e in m.ordered
        if (m.module(e), e["name"]) not in src_names
    ]
    if additions:
        print(
            f"gen_abi_hostcalls: AOT table completes the live set (safe, "
            f"name-resolved additions): {additions}",
            file=sys.stderr,
        )
    return problems


def verify_ring3(m):
    """Verify each hc_id in the live warp_ring3_dispatch switch calls the same
    warp_* wrapper the IDL derives. (Per-arg register placement is validated
    behaviorally by run-qemu-test at swap time; the generator fixes the known
    proc_info_stats ctx-register bug, so that case is expected to differ.)"""
    text = _read(WARP_LINK_CPP)
    region = text.split("switch (hc_id)", 1)[1].split("\n    default:", 1)[0]
    region = re.sub(r"/\*.*?\*/", " ", region, flags=re.S)
    # Associate each warp_* call with the case labels that precede it (handles
    # fall-through: multiple labels share one call).
    src, pending = {}, []
    for label, fn in re.findall(r"case\s+(HC_[A-Z0-9_]+)\s*:|(warp_\w+)\s*\(", region):
        if label:
            pending.append(label)
        elif fn:
            for lbl in pending:
                src[lbl] = fn
            pending = []
    problems = []
    for e in m.ordered:
        sym = hc_symbol(e)
        want = m.warp_fn(e)
        got = src.get(sym)
        if got is None:
            problems.append(f"ring-3 dispatch: no case for {sym}")
        elif got != want:
            problems.append(f"ring-3 dispatch {sym}: calls {got}, IDL derives {want}")
    return problems


def verify_source(m):
    problems = (
        verify_enum(m)
        + verify_warp(m)
        + verify_wasm3(m)
        + verify_aot(m)
        + verify_ring3(m)
    )
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
