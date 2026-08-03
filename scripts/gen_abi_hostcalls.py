#!/usr/bin/env python3
"""Generate the WASM host-call ABI from abi/hostcalls.yaml.

Kernel-side surfaces (into abi/generated/c/):
  - wasmos_hostcall_ids.h    the kernel HC_* id enum
  - wasmos_symbols_warp.inc  the WARP WASMOS_SYMBOLS(LINK) table
  - wasmos_link_wasm3.inc    the wasm3 link X-macro WASMOS_WASM3_LINKS(X) (m3 sigs)
  - wasmos_symbols_aot.inc   the WARP AOT symbol table WASMOS_AOT_SYMBOLS(LINK)
  - wasmos_ring3_dispatch.inc  warp_ring3_dispatch_table() (arg-unpack switch)

These are all live in the kernel/AOT-tool now (link.cpp/link.c/warp_ring3.h/
warp_aot.cpp #include them); only the wrapper *bodies* stay hand-written.

Client-side guest import stubs (into abi/generated/<lang>/wasmos_imports.<ext>):
  - rust/wasmos_imports.rs            #[link(wasm_import_module="wasmos")] extern
  - go/wasmos_imports.go              //go:wasmimport wasmos <sym>
  - zig/wasmos_imports.zig            pub extern "wasmos" fn … callconv(.c)
  - assemblyscript/wasmos_imports.ts  @external("wasmos", …) declare function
Every "wasmos"-module host call (incl. aliases) as its raw wasm ABI signature
(all params i32, i32 return). These are authoritative bindings a guest app opts
into per symbol; the wasi/env-module calls are toolchain-provided, not ours to
declare. C is deliberately NOT regenerated: src/libc/include/wasmos/api.h is a
hand-ergonomic surface (typed pointers, struct params, doc comments) that would
lose those types under codegen — instead --verify-source guards it against drift.

See docs/architecture/34-abi-idl-and-error-model.md.

ID guarantees enforced by the Model: ids unique, dense 0..N-1, ordered
(retired slots must be explicit `reserved: true`), HC symbols unique.

Usage:
    gen_abi_hostcalls.py [--check] [--verify-source]
      (no args)        regenerate all generated host-call files
      --check          fail (exit 2) if any on-disk file differs from the IDL
      --verify-source  fail (exit 3) if the IDL does not reproduce the live
                       hand-written sources. The kernel-table checks self-skip
                       once a surface is swapped to the generated include (all
                       are now), so the enduring job is the C client guard:
                       every WASMOS_WASM_IMPORT("wasmos", …) decl in src/libc +
                       src/libsys must name a real IDL host call with a matching
                       arity (so api.h can never silently drift from the IDL)
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
C_IMPORTS_OUT = os.path.join(GEN_DIR, "wasmos_imports.h")

GEN_ROOT = os.path.join(REPO_ROOT, "abi", "generated")
DOCS_OUT = os.path.join(GEN_ROOT, "docs", "hostcalls.md")
RUST_OUT = os.path.join(GEN_ROOT, "rust", "wasmos_imports.rs")
GO_OUT = os.path.join(GEN_ROOT, "go", "wasmos_imports.go")
ZIG_OUT = os.path.join(GEN_ROOT, "zig", "wasmos_imports.zig")
AS_OUT = os.path.join(GEN_ROOT, "assemblyscript", "wasmos_imports.ts")

# Where hand-written C client import decls live (guarded, not regenerated).
C_CLIENT_DIRS = [
    os.path.join(REPO_ROOT, "src", "libc"),
    os.path.join(REPO_ROOT, "src", "libsys"),
]
# WASM import names that api.h declares but that are NOT WASM host calls: they
# are native driver_api vtable entries (native_driver.c) reached only on the
# native build via the WASMOS_WASM_IMPORT no-op shim. Pending the futex-backed
# user-mutex migration (TODO(user-mutex-futex) in wasmos/mutex.h).
C_CLIENT_ALLOWLIST = {"mutex_try_lock", "mutex_unlock"}

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

    def client_hostcalls(self):
        """The guest-importable "wasmos"-module host calls (incl. aliases), in id
        order. wasi/env-module calls are toolchain-provided, not ours to declare;
        reserved slots are skipped."""
        return [
            e
            for e in self.ordered
            if self.module(e) == "wasmos" and not e.get("reserved")
        ]

    def client_note(self, e):
        """A trailing per-symbol note for a client binding: alias target and/or a
        runtime restriction (a call linked in only one runtime is unresolved if a
        guest on the other runtime imports it)."""
        bits = []
        if "alias_of" in e:
            bits.append(f"alias of {e['alias_of']}")
        rt = self.runtimes(e)
        if set(rt) != {"warp", "wasm3"}:
            bits.append(f"{'/'.join(rt)}-only")
        return f"  // {'; '.join(bits)}" if bits else ""


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
    """WARP AOT symbol table — mirrors WASMOS_SYMBOLS in id order (position == id;
    rebind is positional). Each entry binds an arity- AND return-matched stub so
    its type signature matches the kernel wrapper: stub_i<N> for a uint32_t
    return, stub_v<N> for void (N = wasm param count). initFromCompiledBinary()
    rebinds the stub to the live kernel fn. Emitted as a macro the AOT tool
    expands into its NativeSymbol[] initializer."""
    o = list(BANNER)
    w = o.append
    w("/* WARP AOT symbol table — mirrors WASMOS_SYMBOLS in id order (position ==")
    w(" * id; rebind is positional). Each entry binds an arity- AND return-matched")
    w(" * stub so the type signature matches the kernel wrapper: stub_i<N> for a")
    w(" * uint32_t return, stub_v<N> for void; N = wasm param count (ctx excluded).")
    w(" * initFromCompiledBinary() rebinds the stub to the live kernel fn. Expand:")
    w(" *   static vb::NativeSymbol syms[] = { WASMOS_AOT_SYMBOLS(DYNAMIC_LINK) }; */")
    w("#define WASMOS_AOT_SYMBOLS(LINK) \\")
    lines = [
        f'    LINK("{m.module(e)}", "{e["name"]}", '
        f'stub_{"v" if m.returns(e) == "void" else "i"}{m.arity(e)})'
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


# --------------------------------------------------------------------------- client stubs

_GEN_LINE = (
    "GENERATED by scripts/gen_abi_hostcalls.py from abi/hostcalls.yaml — DO NOT EDIT."
)
_REF_LINE = "See docs/architecture/34-abi-idl-and-error-model.md."


def _line_banner():
    return [f"// {_GEN_LINE}", f"// {_REF_LINE}"]


def _pascal(name):
    return "".join(p.capitalize() for p in name.split("_"))


def _doc_block_c(text, indent=""):
    """A `doc:` string as a C block comment (empty list if no doc)."""
    if not text:
        return []
    lines = text.rstrip("\n").split("\n")
    if len(lines) == 1:
        return [f"{indent}/* {lines[0]} */"]
    out = [f"{indent}/* {lines[0]}"]
    out += [f"{indent} * {ln}".rstrip() for ln in lines[1:]]
    out.append(f"{indent} */")
    return out


def _doc_lines(text, prefix):
    """A `doc:` string as line comments with the given prefix (e.g. `///`, `//`)."""
    if not text:
        return []
    return [f"{prefix} {ln}".rstrip() for ln in text.rstrip("\n").split("\n")]


def emit_c_client(m):
    """C guest import decls for the "wasmos" module. api.h #includes this after its
    struct typedefs + the WASMOS_WASM_IMPORT duality macro. Param C types default
    to int32_t (a wasm32 pointer crosses as an i32 offset); a param may override
    with `c_type` to keep an ergonomic typed signature (const char*, a struct*,
    uint64_t*). The native-only mutex_* pair and the toolchain-provided wasi/env
    imports are declared elsewhere, not here."""
    o = list(BANNER)
    w = o.append
    w("#ifndef WASMOS_GENERATED_CLIENT_IMPORTS_H")
    w("#define WASMOS_GENERATED_CLIENT_IMPORTS_H")
    w("/* Included by src/libc/include/wasmos/api.h AFTER <stdint.h>, the")
    w(" * WASMOS_WASM_IMPORT macro (wasmos/imports.h), and the struct typedefs")
    w(" * referenced below (wasmos_physmem_stats_t, wasmos_framebuffer_info_t). */")
    w("")
    for e in m.client_hostcalls():
        o.extend(_doc_block_c(e.get("doc", "")))
        ret = "void" if m.returns(e) == "void" else "int32_t"
        t = m._target(e)
        ps = t.get("params") or []
        params = (
            "void"
            if not ps
            else ", ".join(f"{p.get('c_type', 'int32_t')} {p['name']}" for p in ps)
        )
        w(
            f'extern {ret} wasmos_{e["name"]}({params}) '
            f'WASMOS_WASM_IMPORT("wasmos", "{e["name"]}");'
        )
    w("")
    w("#endif /* WASMOS_GENERATED_CLIENT_IMPORTS_H */")
    w("")
    return "\n".join(o)


def emit_rust(m):
    """Rust guest bindings — one extern block linking the "wasmos" import module.
    The fn name is the import name, so it must equal the host-call symbol."""
    o = _line_banner()
    o += [
        "",
        "#![allow(dead_code)]",
        "",
        '#[link(wasm_import_module = "wasmos")]',
        'unsafe extern "C" {',
    ]
    for e in m.client_hostcalls():
        o.extend(_doc_lines(e.get("doc", ""), "    ///"))
        params = ", ".join(f"a{i}: i32" for i in range(m.arity(e)))
        ret = "" if m.returns(e) == "void" else " -> i32"
        o.append(f"    pub fn {e['name']}({params}){ret};{m.client_note(e)}")
    o += ["}", ""]
    return "\n".join(o)


def emit_go(m):
    """Go/TinyGo guest bindings. //go:wasmimport carries the import name, so the
    exported PascalCase func name is free-form; the file imports unsafe as the
    directive requires."""
    o = _line_banner()
    o += [
        "",
        "package wasmos",
        "",
        'import _ "unsafe" // required by //go:wasmimport',
        "",
    ]
    for e in m.client_hostcalls():
        params = ", ".join(f"a{i} int32" for i in range(m.arity(e)))
        ret = "" if m.returns(e) == "void" else " int32"
        o.extend(_doc_lines(e.get("doc", ""), "//"))
        note = m.client_note(e)
        if note:
            o.append(note.strip())  # a clean "// …" line; keep //go: directive bare
        o.append(f"//go:wasmimport wasmos {e['name']}")
        o.append(f"func {_pascal(e['name'])}({params}){ret}")
        o.append("")
    return "\n".join(o)


def emit_zig(m):
    """Zig guest bindings — the extern module string is the import module and the
    fn name is the import name."""
    o = _line_banner()
    o += [""]
    for e in m.client_hostcalls():
        o.extend(_doc_lines(e.get("doc", ""), "///"))
        params = ", ".join(f"a{i}: i32" for i in range(m.arity(e)))
        ret = "void" if m.returns(e) == "void" else "i32"
        o.append(
            f'pub extern "wasmos" fn {e["name"]}({params}) callconv(.c) {ret};'
            f"{m.client_note(e)}"
        )
    o += [""]
    return "\n".join(o)


def emit_as(m):
    """AssemblyScript guest bindings — @external names the (module, import)."""
    o = _line_banner()
    o += [""]
    for e in m.client_hostcalls():
        o.extend(_doc_lines(e.get("doc", ""), "//"))
        params = ", ".join(f"a{i}: i32" for i in range(m.arity(e)))
        ret = "void" if m.returns(e) == "void" else "i32"
        o.append(f'@external("wasmos", "{e["name"]}")')
        o.append(
            f"export declare function {e['name']}({params}): {ret};{m.client_note(e)}"
        )
    o += [""]
    return "\n".join(o)


def emit_docs(m):
    """Markdown host-call reference, grouped by module."""
    o = [
        "<!-- GENERATED by scripts/gen_abi_hostcalls.py from abi/hostcalls.yaml — DO NOT EDIT. -->",
        "# WASMOS Host Calls",
        "",
        f"{m.count} host calls (ids 0..{m.count - 1}). Guests reach these as `wasmos.*` /",
        "`wasi_snapshot_preview1` / `env` imports; the C client is `wasmos_<name>`.",
        "",
    ]
    modules = ["wasmos", "wasi_snapshot_preview1", "env"]
    for mod in modules:
        entries = [e for e in m.ordered if m.module(e) == mod]
        if not entries:
            continue
        o.append(f"## `{mod}`")
        o.append("")
        o.append("| id | name | params | returns | description |")
        o.append("|---|---|---|---|---|")
        for e in entries:
            if e.get("reserved"):
                o.append(
                    f"| {e['id']} | *(reserved)* | | | retired; slot kept for id stability |"
                )
                continue
            t = m._target(e)
            params = (
                ", ".join(f"{p['name']}:{p['kind']}" for p in (t.get("params") or []))
                or "—"
            )
            doc = e.get("doc") or ""
            doc = " ".join(doc.split()).replace("|", "\\|")
            alias = f" (alias of {e['alias_of']})" if "alias_of" in e else ""
            o.append(
                f"| {e['id']} | `{e['name']}`{alias} | {params} | {m.returns(e) or '—'} | {doc} |"
            )
        o.append("")
    return "\n".join(o)


OUTPUTS = [
    (IDS_OUT, emit_hostcall_ids),
    (WARP_OUT, emit_warp_symbols),
    (WASM3_OUT, emit_wasm3_links),
    (AOT_OUT, emit_aot_symbols),
    (RING3_OUT, emit_ring3_dispatch),
    (C_IMPORTS_OUT, emit_c_client),
    (DOCS_OUT, emit_docs),
    (RUST_OUT, emit_rust),
    (GO_OUT, emit_go),
    (ZIG_OUT, emit_zig),
    (AS_OUT, emit_as),
]


# --------------------------------------------------------------------------- verify


def _read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def verify_enum(m):
    text = _read(WARP_RING3_H)
    src = {s: int(v) for s, v in re.findall(r"\b(HC_[A-Z0-9_]+)\s*=\s*(\d+)", text)}
    if not src:
        return []  # enum already swapped to the generated header; --check guards it
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
    if "#define WASMOS_SYMBOLS(LINK)" not in text:
        return []  # WASMOS_SYMBOLS already swapped to the generated include
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
    if not src:
        return []  # wasm3 link table already swapped to the generated include
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
    if "WASMOS_AOT_SYMBOLS(" in text:
        return []  # AOT table already swapped to the generated macro; --check guards it
    region = text.split("aot_symbols", 1)[1]
    region = re.sub(r"/\*.*?\*/", " ", region, flags=re.S)
    region = region.replace("\\", " ")
    # Match both stub_i<N> (uint32_t return) and stub_v<N> (void return).
    src = re.findall(
        r'DYNAMIC_LINK\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(stub_[iv]\d+)\s*\)', region
    )
    gen = [
        (
            m.module(e),
            e["name"],
            f'stub_{"v" if m.returns(e) == "void" else "i"}{m.arity(e)}',
        )
        for e in m.ordered
    ]
    problems = []
    if len(src) != len(gen):
        problems.append(f"AOT table length differs: src={len(src)} idl={len(gen)}")
    for i, (s, g) in enumerate(zip(src, gen)):
        if s != g:
            problems.append(f"AOT entry {i} differs: src={s} idl={g}")
    return problems


def verify_ring3(m):
    """Verify each hc_id in the live warp_ring3_dispatch switch calls the same
    warp_* wrapper the IDL derives. (Per-arg register placement is validated
    behaviorally by run-qemu-test at swap time; the generator fixes the known
    proc_info_stats ctx-register bug, so that case is expected to differ.)"""
    text = _read(WARP_LINK_CPP)
    if "switch (hc_id)" not in text:
        return []  # ring-3 dispatch already swapped to warp_ring3_dispatch_table
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


_C_IMPORT_RE = re.compile(
    r'([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*WASMOS_WASM_IMPORT\(\s*"wasmos"\s*,\s*"([^"]+)"\s*\)',
    re.S,
)


def _c_client_imports():
    """Every `<fn>(<params>) WASMOS_WASM_IMPORT("wasmos", "<sym>")` decl in the
    hand-written libc/libsys headers, as (sym, arity, relpath). Params here are
    flat (scalars + one-level pointers, no function-pointer params), so a
    top-level comma count is the arity; `(void)`/empty is zero."""
    out = []
    for base in C_CLIENT_DIRS:
        for dp, _dirs, files in os.walk(base):
            for f in files:
                if not f.endswith((".h", ".hpp", ".c", ".cpp")):
                    continue
                path = os.path.join(dp, f)
                for fn, params, sym in _C_IMPORT_RE.findall(_read(path)):
                    p = params.strip()
                    arity = 0 if p in ("", "void") else p.count(",") + 1
                    out.append((sym, arity, os.path.relpath(path, REPO_ROOT), fn))
    return out


def verify_c_client(m):
    """Guard the hand-written C client (api.h et al.) against IDL drift: every
    WASM import it declares must name a real host call with a matching arity.
    The C client is a *subset* (native-only / toolchain-provided calls have no
    libc decl), so missing coverage is fine — only wrong or stale decls fail."""
    idl = {e["name"]: m.arity(e) for e in m.ordered if m.module(e) == "wasmos"}
    problems = []
    for sym, arity, rel, fn in _c_client_imports():
        if sym in C_CLIENT_ALLOWLIST:
            continue
        if sym not in idl:
            problems.append(
                f"C client declares WASM import '{sym}' ({fn}, {rel}) that is not "
                f"an IDL host call (stale/typo, or add it to the IDL)"
            )
        elif arity != idl[sym]:
            problems.append(
                f"C client '{sym}' arity {arity} != IDL {idl[sym]} ({fn}, {rel})"
            )
    return problems


def verify_source(m):
    problems = (
        verify_enum(m)
        + verify_warp(m)
        + verify_wasm3(m)
        + verify_aot(m)
        + verify_ring3(m)
        + verify_c_client(m)
    )
    if problems:
        for p in problems:
            print(f"gen_abi_hostcalls: {p}", file=sys.stderr)
        die("IDL does not match the live sources — reconcile before generating", code=3)
    print(
        f"gen_abi_hostcalls: IDL verified against the hand-written host-call sources "
        f"(swapped-in kernel tables self-skip; C client decls guarded against drift; "
        f"{m.count} host calls, ids 0..{m.count - 1})"
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

    for path, emit in OUTPUTS:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(emit(model))
        print(f"gen_abi_hostcalls: wrote {os.path.relpath(path, REPO_ROOT)}")


if __name__ == "__main__":
    main()
