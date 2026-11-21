"""WASMOS-specific build helpers shared by manifests: guest modules, native
ring-3 modules, packing, ESP installation and kernel blobs.

buildgen.py is project-agnostic; anything that knows about the SDK drivers,
the packer, WARP AOT or the ESP layout lives here.
"""

from __future__ import annotations

from pathlib import Path

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
# nothing, as the CMake build does.
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


def _manifest_wants_aot(manifest: Path) -> bool:
    try:
        return "aot = true" in Path(manifest).read_text()
    except OSError:
        return False


def pack(b, name, image, manifest, aot=None):
    """Pack a module image (.wasm or native .elf) into <name>.wap.

    Under WARP, a wasm module whose manifest says `aot = true` is compiled
    ahead of time by warp_aot and the .warpbin is bundled next to the bytecode.
    `aot=False` forces the plain path (native images are never AOT-compiled)."""
    sdk = b.exports["sdk"]
    manifest = Path(manifest)
    b.regen_on(manifest)  # `aot = true` is read at generation time
    wap = f"{name}.wap"
    argv = [_run(sdk["packer"]), "--manifest", manifest, "--in", image]
    implicit = [manifest, sdk["packer"]]
    if aot is None:
        aot = _manifest_wants_aot(manifest)
    warp_aot = b.exports.get("warp_aot")
    if aot and warp_aot:
        warpbin = f"{name}.warpbin"
        b.command(
            [warpbin],
            [_run(warp_aot), image, warpbin],
            inputs=[image],
            implicit=[warp_aot],
            description=f"AOT {warpbin}",
        )
        argv += ["--compiled", warpbin]
        implicit.append(warpbin)
    argv += ["--out", wap]
    b.command([wap], argv, inputs=[image], implicit=implicit, description=f"PACK {wap}")
    b.alias("modules", wap)
    return wap


def guest(
    b, name, sources, manifest=None, lang="c", flags=(), wasm=None, aot=None, cxx=False
):
    """Build a guest module with the staged SDK and pack it into <name>.wap.

    `manifest` defaults to linker.metadata beside the manifest file; its
    [link] section sizes the module, so it is an input. C sources compile one
    object each with an exact header depfile and link in source order (the
    order is part of the image); other languages are a single driver call
    whose inputs are the listed sources. Returns the .wap path, or None when
    the language is disabled or its toolchain is missing."""
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
        b.command(
            [wasm],
            head + ["--emit-wasm", f"--wasmos-manifest={manifest}", *objs, "-o", wasm],
            inputs=objs,
            implicit=deps,
            description=f"LINK[guest] {wasm}",
        )
    else:
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
    """Compile a native Zig root module to an x86_64 object.

    `includes` are the root module's -I directories. `modules` maps each
    dependency module name to (root file, its -I directories); each is passed as
    `--dep <name>` and defined with `-M<name>=<file>` after its own includes.
    Zig emits no depfile, so `implicit` lists the files the object depends on
    beyond the module roots."""
    argv = [
        b.tools["zig"],
        "build-obj",
        "-target",
        "x86_64-freestanding",
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
    """Wrap a build-dir file as an x86_64 ELF object for the kernel link.

    llvm-objcopy names the _binary_<file>_start/_end symbols after the input
    path as given, so it runs in the build directory on the bare filename."""
    payload = Path(payload)
    blob = f"{payload.stem}_blob.o"
    b.command(
        [blob],
        [
            b.tools["llvm-objcopy"],
            "--input-target=binary",
            "--output-target=elf64-x86-64",
            "--binary-architecture=i386:x86-64",
            str(payload),
            blob,
        ],
        inputs=[str(payload)],
        description=f"BLOB {blob}",
    )
    return blob
