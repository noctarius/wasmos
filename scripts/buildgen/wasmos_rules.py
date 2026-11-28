"""WASMOS-specific build helpers shared by manifests: guest modules, native
ring-3 modules, packing, ESP installation and kernel blobs.

buildgen.py is project-agnostic; anything that knows about the SDK drivers,
the packer, WARP AOT or the ESP layout lives here.
"""

from __future__ import annotations

import tomllib
from pathlib import Path

from toolchains import arch

# SDK drivers by source language. Every one accepts
# `--emit-wasm --wasmos-manifest=<m> <sources> -o <x>.wasm`.
DRIVERS = {
    "c": "wasmos-clang",
    "zig": "wasmos-zig",
    "as": "wasmos-asc",
    "rust": "wasmos-rustc",
    "go": "wasmos-tinygo",
}


# Kconfig switch and required tools per optional guest language. C is always
# available; a language whose switch is off or whose tool is missing builds
# nothing.
LANG_GATES = {
    "zig": ("ZIG_ENABLE", ["zig"]),
    "as": ("AS_ENABLE", ["asc"]),
    "rust": ("RUST_ENABLE", ["rustc"]),
    "go": ("GO_ENABLE", ["tinygo", "wasm-opt"]),
}


def lang_available(b, lang: str) -> bool:
    gate = LANG_GATES.get(lang)
    if gate is None:
        return True
    symbol, tools = gate
    return bool(b.cfg.get(symbol, True)) and all(b.tools.get(t) for t in tools)


def _run(tool: str) -> str:
    """argv[0] spelling of a tool built into the build directory: a bare name
    would be looked up on PATH."""
    return tool if "/" in tool else f"./{tool}"


def _manifest_aot(manifest: Path):
    """The manifest's [package] `aot` setting: "all" for `aot = "all"`, True for
    `aot = true`, False when it is false or absent. Any other value stops the
    generation rather than quietly packing no AOT code."""
    try:
        package = tomllib.loads(Path(manifest).read_text()).get("package", {})
    except OSError:
        return False
    value = package.get("aot", False)
    if value is True or value is False or value == "all":
        return value
    raise SystemExit(
        f'{manifest}: [package] aot must be true, false or "all", not {value!r}'
    )


# Capability kinds a publisher's key may grant (capability_kind_for_apps in
# src/kernel/capability.c); ipc.basic is everyone's.
APP_KINDS = {"ipc.basic", "dma.buffer"}


def _initfs_packages(b):
    """The package files the tree's initfs image carries
    (scripts/initfs.<arch>.toml): started before the keyring exists."""
    toml = b.root / f"scripts/initfs.{arch(b.cfg).name}.toml"
    b.regen_on(toml)
    try:
        entries = tomllib.loads(toml.read_text())
    except OSError:
        return set()
    return {
        e.get("source", "")
        for table in entries.values()
        if isinstance(table, list)
        for e in table
        if isinstance(e, dict)
    }


def _signing_seed(b, name, manifest: Path):
    """The seed a package is signed with: the build's app key for an examples/
    package whose manifest asks only for app kinds (APP_KINDS) and that the
    initfs image does not carry, the system key for everything else. The kernel
    trusts the app key only once the keyring has registered it, and grants it the
    app kinds only, so a demo app that needs a critical kind -- system.control,
    mmio.map -- or that boot starts before the keyring is signed with the system
    key like the system's own packages (docs/architecture/10, "Signed
    Packages")."""
    try:
        examples = Path(manifest).resolve().is_relative_to(b.root / "examples")
        caps = tomllib.loads(Path(manifest).read_text()).get("capabilities", [])
    except OSError:
        return b.exports["signing.seed"]
    if (
        examples
        and all(c.get("name") in APP_KINDS for c in caps)
        and f"{name}.wap" not in _initfs_packages(b)
    ):
        return b.exports["signing.app_seed"]
    return b.exports["signing.seed"]


def pack(b, name, image, manifest, aot=None, sign=True):
    """Pack a module image (.wasm or native .elf) into <name>.wap.

    Under WARP, a wasm module whose manifest says `aot = true` is compiled
    ahead of time for the tree's architecture and the .warpbin is bundled next
    to the bytecode, tagged with that architecture. `aot = "all"` bundles one per
    architecture (a "fat" package that runs from AOT code on either); system
    modules ship with the kernel that runs them and need only their own.
    `aot="foreign"` bundles every architecture EXCEPT the tree's: a test fixture
    for a package this kernel has no AOT binary for.
    `aot=False` forces the plain path (native images are never AOT-compiled).
    The package is signed with one of the build's keys (_signing_seed) unless
    `sign=False`, or with the seed file `sign` names when it is a path: both only
    a test of the signature policy wants."""
    sdk = b.exports["sdk"]
    manifest = Path(manifest)
    b.regen_on(manifest)  # `aot = true` is read at generation time
    wap = f"{name}.wap"
    argv = [_run(sdk["packer"]), "--manifest", manifest, "--in", image]
    implicit = [manifest, sdk["packer"]]
    if aot is None:
        aot = _manifest_aot(manifest)
    warp_aot = b.exports.get("warp_aot")
    if aot and warp_aot:
        tools = warp_aot["tools"]
        if aot == "all":
            arches = sorted(tools)
        elif aot == "foreign":
            arches = sorted(a for a in tools if a != warp_aot["tree"])
        else:
            arches = [warp_aot["tree"]]
        for arch in arches:
            warpbin = f"{name}.{arch}.warpbin"
            b.command(
                [warpbin],
                [_run(tools[arch]), image, warpbin],
                inputs=[image],
                implicit=[tools[arch]],
                description=f"AOT {warpbin}",
            )
            argv += ["--compiled", f"{arch}={warpbin}"]
            implicit.append(warpbin)
    # The kernel grants a privileged capability only to a package signed with
    # the build's key (docs/architecture/10-capability-and-policy.md).
    if sign:
        seed = sign if isinstance(sign, Path) else _signing_seed(b, name, manifest)
        argv += ["--sign", seed]
        implicit.append(seed)
    argv += ["--out", wap]
    b.command([wap], argv, inputs=[image], implicit=implicit, description=f"PACK {wap}")
    b.alias("modules", wap)
    return wap


def guest(
    b,
    name,
    sources,
    manifest=None,
    lang="c",
    flags=(),
    wasm=None,
    aot=None,
    cxx=False,
    libs=(),
):
    """Build a guest module with the staged SDK and pack it into <name>.wap.

    `manifest` defaults to linker.metadata beside the manifest file; its
    [link] section sizes the module, so it is an input. C sources compile one
    object each with an exact header depfile and link in source order (the
    order is part of the image); other languages are a single driver call
    whose inputs are the listed sources. `libs` are link inputs for C modules
    (e.g. "-lmonocypher" for an SDK library), placed after the objects.
    Returns the .wap path, or None when the language is disabled or its
    toolchain is missing."""
    if not lang_available(b, lang):
        return None
    sdk = b.exports["sdk"]
    manifest = Path(manifest) if manifest else b.src("linker.metadata")
    sources = [Path(s) for s in sources]
    wasm = wasm or f"{name}.wasm"
    driver = f"{sdk['bin']}/{DRIVERS[lang]}"
    head = [driver]
    if cxx:
        head.append("--wasmos-cxx")
    deps = [manifest, *sdk["deps"]]

    if lang == "c":
        cflags = [f"-DWASMOS_TRACE={b.cfg.flag('WASMOS_TRACE')}", *flags]
        objs = []
        for src in sources:
            obj = b.obj_path(src, subdir=f"obj/guest/{name}")
            b.command(
                [obj],
                head + cflags + ["-MD", "-MF", f"{obj}.d", "-c", src, "-o", obj],
                inputs=[src],
                implicit=sdk["deps"],
                depfile=f"{obj}.d",
                description=f"CC[guest] {obj}",
            )
            objs.append(obj)
            # The driver is a wrapper over the staged sysroot. Index the same
            # compile against the in-tree headers the sysroot is staged from, so
            # tools resolve (and diagnose) the real files without a staged SDK.
            wasm32 = b.toolchain("wasm32").tc
            b.index(src, [wasm32.cxx if cxx else wasm32.cc, *wasm32.cflags, *cflags])
        b.command(
            [wasm],
            head
            + [
                "--emit-wasm",
                f"--wasmos-manifest={manifest}",
                *objs,
                *libs,
                "-o",
                wasm,
            ],
            inputs=objs,
            implicit=deps,
            description=f"LINK[guest] {wasm}",
        )
    else:
        # C sources a Zig/Rust/Go driver compiles internally, indexed against
        # the in-tree headers.
        wasm32 = b.toolchain("wasm32").tc
        for src in sources:
            if src.suffix == ".c":
                b.index(src, [wasm32.cc, *wasm32.cflags, *flags])
        b.command(
            [wasm],
            head
            + [
                "--emit-wasm",
                f"--wasmos-manifest={manifest}",
                *flags,
                *sources,
                "-o",
                wasm,
            ],
            inputs=sources,
            implicit=deps,
            description=f"WASM[{lang}] {wasm}",
        )
    return pack(b, name, wasm, manifest, aot=aot)


def native(b, name, objs, entry, manifest=None, image_base=None, elf=None):
    """Link native ring-3 objects into a static ELF and pack it into
    <name>.wap. Native images are never AOT-compiled. Returns the .wap path."""
    manifest = Path(manifest) if manifest else b.src("linker.metadata")
    elf = elf or f"{name}.elf"
    argv = [b.tools["ld.lld"]]
    if image_base:
        argv.append(f"--image-base={image_base}")
    argv += ["-e", entry, "-static", *objs, "-o", elf]
    b.command([elf], argv, inputs=objs, description=f"LINK[native] {elf}")
    return pack(b, name, elf, manifest, aot=False)


def zig_object(b, out, root, includes, modules, implicit=()):
    """Compile a native Zig root module to an object for the target architecture.

    `includes` are the root module's -I directories. `modules` maps each
    dependency module name to (root file, its -I directories); each is passed as
    `--dep <name>` and defined with `-M<name>=<file>` after its own includes.
    Zig emits no depfile, so `implicit` lists the files the object depends on
    beyond the module roots."""
    argv = [
        b.tools["zig"],
        "build-obj",
        "-target",
        arch(b.cfg).zig_target,
        "-O",
        "ReleaseSmall",
        "-fstrip",
    ]
    for dep in modules:
        argv += ["--dep", dep]
    argv += [x for d in includes for x in ("-I", str(d))] + [f"-Mroot={root}"]
    for dep, (path, dep_includes) in modules.items():
        argv += [x for d in dep_includes for x in ("-I", str(d))] + [f"-M{dep}={path}"]
    argv.append(f"-femit-bin={out}")
    roots = [path for path, _ in modules.values()]
    b.command(
        [out],
        argv,
        inputs=[root],
        implicit=[*roots, *implicit],
        description=f"ZIG {out}",
    )
    return out


def asc_tool(b, name, source, wasm, manifest=None):
    """Build a single-file AssemblyScript tool with asc directly: the source is
    staged as app.ts beside the libc's wasmos.ts behind runtime.ts (as entry.ts),
    the layout runtime.ts imports. Returns the .wap path, or None without asc."""
    if not lang_available(b, "as"):
        return None
    libc = b.root / "src/libc/assemblyscript"
    stage = f"asc_{name}_src"
    b.command(
        [wasm],
        [
            ["mkdir", "-p", stage],
            ["cp", source, f"{stage}/app.ts"],
            ["cp", libc / "wasmos.ts", f"{stage}/wasmos.ts"],
            ["cp", libc / "runtime.ts", f"{stage}/entry.ts"],
            [
                b.tools["asc"],
                f"{stage}/entry.ts",
                "--target",
                "release",
                "-Osize",
                "--runtime",
                "stub",
                "--noAssert",
                "--outFile",
                wasm,
            ],
        ],
        inputs=[source],
        implicit=[libc / "wasmos.ts", libc / "runtime.ts"],
        description=f"ASC {wasm}",
    )
    return pack(b, name, wasm, manifest or b.src("linker.metadata"))


def install(b, dest, path):
    """Stage a build output or source file at `dest` on the ESP (a path relative
    to the ESP root, e.g. "system/drivers/ata.wap"). A None path (a module whose
    language is unavailable) stages nothing."""
    if path is None:
        return
    b.exports.setdefault("esp.files", []).append((dest, str(path)))


def kernel_blob(b, payload):
    """Wrap a build-dir file as an ELF object for the kernel link.

    llvm-objcopy names the _binary_<file>_start/_end symbols after the input
    path as given, so it runs in the build directory on the bare filename."""
    payload = Path(payload)
    blob = f"{payload.stem}_blob.o"
    b.command(
        [blob],
        [
            b.tools["llvm-objcopy"],
            "--input-target=binary",
            f"--output-target={arch(b.cfg).objcopy_target}",
            f"--binary-architecture={arch(b.cfg).objcopy_binary_arch}",
            str(payload),
            blob,
        ],
        inputs=[str(payload)],
        description=f"BLOB {blob}",
    )
    return blob
