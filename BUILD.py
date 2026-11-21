"""Top-level manifest: host tools, then components in dependency order.

Exports flow downward: a manifest may read what an earlier one published in
b.exports, so the subdir order below is the dependency order."""


def build(b):
    host = b.toolchain("host")
    packer = host.link(
        "make_wasmos_app", host.compile([b.src("scripts/make_wasmos_app.c")])
    )
    b.exports["packer"] = packer
    b.alias("make_wasmos_app", packer)

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
