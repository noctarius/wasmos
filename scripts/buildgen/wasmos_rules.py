"""WASMOS-specific build helpers shared by manifests (guest modules, blobs).

buildgen.py is project-agnostic; anything that knows about the SDK, the packer
or the kernel's blob convention lives here.
"""

from __future__ import annotations

from pathlib import Path


def wasm_c_app(b, name, sources, manifest, flags=(), pack=True):
    """Build a C guest module with the staged SDK's wasmos-clang, and pack it
    into a .wap unless pack=False. Returns (wasm, wap-or-None).

    The manifest's [link] section sizes the module, so it is an input; a
    single-source module also gets an exact header depfile."""
    sdk = b.exports["sdk"]
    wasm = f"{name}.wasm"
    argv = [
        sdk["clang"],
        "--emit-wasm",
        f"--wasmos-manifest={manifest}",
        f"-DWASMOS_TRACE={b.cfg.flag('WASMOS_TRACE')}",
        *flags,
    ]
    depfile = None
    if len(sources) == 1:
        depfile = f"{wasm}.d"
        argv += ["-MD", "-MF", depfile]
    argv += [str(s) for s in sources] + ["-o", wasm]
    b.command(
        [wasm],
        argv,
        inputs=sources,
        implicit=[manifest, *sdk["deps"]],
        depfile=depfile,
        description=f"WASM {wasm}",
    )
    wap = None
    if pack:
        wap = f"{name}.wap"
        b.command(
            [wap],
            [sdk["packer"], "--manifest", manifest, "--in", wasm, "--out", wap],
            inputs=[wasm],
            implicit=[manifest, sdk["packer"]],
            description=f"PACK {wap}",
        )
    return wasm, wap


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
