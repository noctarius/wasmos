"""Top-level manifest: host tools, then components in dependency order.

Exports flow downward: a manifest may read what an earlier one published in
b.exports, so the subdir order below is the dependency order."""

import os
from pathlib import Path

from wasmos_rules import install


def build(b):
    host = b.toolchain("host")
    # Monocypher (libs/monocypher, a subtree): Ed25519 for signing packages.
    mono = b.root / "libs/monocypher/src"
    b.exports["monocypher"] = {
        "sources": [mono / "monocypher.c", mono / "optional/monocypher-ed25519.c"],
        "include": [f"-I{mono}", f"-I{mono / 'optional'}"],
    }
    packer = host.link(
        "make_wasmos_app",
        host.compile(
            [b.src("scripts/make_wasmos_app.c"), *b.exports["monocypher"]["sources"]],
            flags=b.exports["monocypher"]["include"],
        ),
    )
    b.exports["packer"] = packer
    b.alias("make_wasmos_app", packer)

    # The seed every package is signed with, and the public key the kernel
    # checks signatures against (docs/architecture/10-capability-and-policy.md).
    # Without a configured seed, a development one is created once in the build
    # directory and kept, so rebuilds stay signed with the same key.
    def signing_seed(configured, dev_name):
        if configured:
            return Path(configured).expanduser().resolve()
        dev = b.out / "signing" / dev_name
        if not dev.exists():
            dev.parent.mkdir(parents=True, exist_ok=True)
            dev.write_text(os.urandom(32).hex() + "\n")
            dev.chmod(0o600)
        return dev

    # Two keys. The system key signs the build's packages and is the one key
    # compiled into the kernel; its signature alone grants privilege. The app key
    # is for apps that ask for none: it reaches the kernel only through the
    # keyring, like any publisher's key.
    seed = signing_seed(b.cfg.WASMOS_SIGNING_KEY or "", "dev-seed.hex")
    app_seed = signing_seed(b.cfg.WASMOS_APP_SIGNING_KEY or "", "dev-app-seed.hex")
    b.exports["signing.seed"] = seed
    b.exports["signing.app_seed"] = app_seed
    b.command(
        ["wasmos_signing_pubkey.c"],
        [f"./{packer}", "--pubkey-c", seed, "wasmos_signing_pubkey.c"],
        inputs=[seed],
        implicit=[packer],
        description="KEY wasmos_signing_pubkey.c",
    )
    b.exports["signing.pubkey_c"] = b.out / "wasmos_signing_pubkey.c"
    # Both public keys in the keyring directory, as PEM (docs/architecture/10,
    # "Signed Packages").
    for name, key_seed in (("system", seed), ("apps", app_seed)):
        pem = f"signing/{name}.pem"
        b.command(
            [pem],
            [f"./{packer}", "--pubkey-pem", key_seed, pem],
            inputs=[key_seed],
            implicit=[packer],
            description=f"KEY {pem}",
        )
        install(b, f"system/keys/{name}.pem", b.out / pem)

    # Every linker.metadata's [deps] needs-edges must be acyclic: the packer
    # checks the whole set at once.
    manifests = b.glob("src/**/linker.metadata") + b.glob("examples/**/linker.metadata")
    b.command(
        ["deps-check.stamp"],
        [[f"./{packer}", "--check-deps", *manifests], ["touch", "deps-check.stamp"]],
        inputs=manifests,
        implicit=[packer],
        description="CHECK linker.metadata deps",
    )
    b.alias("check-deps", "deps-check.stamp", default=True)

    b.exports["sdk.libdir"] = "wasmos-sdk/sysroot/lib/wasm32-unknown-wasmos"
    b.exports["sdk.archives"] = []
    b.subdir("src/libc", "src/libsys", "scripts/sdk", "src/tools/warp_aot")
    b.subdir("src/drivers", "src/services", "src/utils")
    b.subdir(
        "examples/c",
        "examples/assemblyscript",
        "examples/rust",
        "examples/go",
        "examples/zig",
    )
    b.subdir("src/kernel", "src/boot", "src/tools/mkfs_wfs")
    b.alias("modules", default=True)
    b.subdir("scripts/system", "tests/unit", "tools/clang_tidy_wasmos")
    quality(b)


# scripts/quality.sh tool variables and the buildgen-tools.json key each reads.
QUALITY_TOOLS = {
    "CLANG_FORMAT": "clang-format",
    "CLANG_TIDY": "clang-tidy",
    "ZIG": "zig",
    "GOFMT": "gofmt",
    "GO": "go",
    "RUSTFMT": "rustfmt",
    "RUSTC": "rustc",
    "ASC": "asc",
    "BLACK": "black",
    "RUFF": "ruff",
}


def quality(b):
    """fmt / fmt-check / lint / quality: scripts/quality.sh owns the source
    allowlist and the per-language tools; this passes it the tools and the
    compile database this tree resolved."""
    env = {"QUALITY_BUILD_DIR": b.out}
    env.update(
        {var: b.tools[key] for var, key in QUALITY_TOOLS.items() if b.tools.get(key)}
    )
    plugin = b.exports.get("tidy_plugin")
    if plugin:
        env["QUALITY_TIDY_PLUGIN"] = b.out / plugin
    script = ["bash", b.root / "scripts/quality.sh"]
    lint_deps = [plugin] if plugin else []
    b.run_target(
        "fmt", script + ["format"], env=env, description="quality: format in place"
    )
    b.run_target(
        "fmt-check",
        script + ["format", "--check"],
        env=env,
        description="quality: check formatting",
    )
    b.run_target(
        "lint", script + ["lint"], deps=lint_deps, env=env, description="quality: lint"
    )
    b.run_target(
        "quality",
        [script + ["format"], script + ["lint"]],
        deps=lint_deps,
        env=env,
        description="quality: format then lint",
    )
