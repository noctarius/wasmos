"""buildgen - generate a Ninja build for WASMOS from Kconfig + BUILD.py manifests.

A build directory is the pair (.config, build.ninja). `configure` seeds .config
once from a defconfig and calls generate(); after that, build.ninja regenerates
itself whenever .config, a Kconfig file, a BUILD.py manifest, a globbed
directory, or this package changes, so `ninja -C <dir>` is the only command a
developer runs.

Manifests are Python files named BUILD.py. Each defines `build(b)` and receives
a Builder scoped to its own directory. They describe WHAT is built (sources,
flags that belong to the component, cross-component outputs); toolchains and the
flags every object of a target class shares live in toolchains.py, so a new
architecture is a new profile there rather than an edit to every manifest.
"""

from __future__ import annotations

import glob as globmod
import json
import os
import runpy
import shlex
import sys
from pathlib import Path

import toolchains

GENERATOR_DIR = Path(__file__).resolve().parent


# --- ninja syntax -----------------------------------------------------------


def _esc_path(p: str) -> str:
    return p.replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def _esc_value(v: str) -> str:
    return v.replace("$", "$$").replace("\n", " ")


def shell_join(args) -> str:
    return " ".join(shlex.quote(str(a)) for a in args)


# --- configuration ----------------------------------------------------------


class Config:
    """Read-only view of a resolved .config.

    Attribute access returns the symbol's value with Kconfig types mapped to
    Python: bool -> bool, int/hex -> int, string -> str. A symbol that is
    absent (undefined, or its dependencies are unmet) reads as False for bools,
    which matches how Kconfig itself treats it.
    """

    def __init__(self, kconfig_path: Path, dotconfig: Path):
        try:
            import kconfiglib
        except ImportError:
            sys.exit(
                "buildgen: kconfiglib is required "
                "(python3 -m pip install kconfiglib)"
            )
        os.environ.setdefault("srctree", str(kconfig_path.parent))
        kconf = kconfiglib.Kconfig(str(kconfig_path), warn_to_stderr=False)
        kconf.load_config(str(dotconfig))
        # Write back the resolved config so defaults and dependency fixups
        # (e.g. SMP without IOAPIC) are visible in the file a developer edits,
        # not only inside this process.
        kconf.write_config(str(dotconfig), header="")
        self._kconf = kconf
        self.kconfig_files = sorted(
            {str(Path(f).resolve()) for f in kconf.kconfig_filenames}
        )

    def get(self, name: str, default=None):
        sym = self._kconf.syms.get(name)
        if sym is None or sym.orig_type == 0:
            return default
        import kconfiglib

        if sym.orig_type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
            return sym.tri_value == 2
        if not sym.visibility and not sym.str_value:
            return default
        if sym.orig_type in (kconfiglib.INT, kconfiglib.HEX):
            return int(sym.str_value, 0) if sym.str_value else default
        return sym.str_value

    def __getattr__(self, name: str):
        if name.startswith("_"):
            raise AttributeError(name)
        return self.get(name, False)

    def flag(self, name: str) -> int:
        """Bool symbol as 0/1, for -DNAME=<0|1> style defines."""
        return 1 if self.get(name, False) else 0


# --- build graph ------------------------------------------------------------


class Graph:
    def __init__(self, srcdir: Path, outdir: Path, cfg: Config, tools: dict):
        self.srcdir = srcdir
        self.outdir = outdir
        self.cfg = cfg
        self.tools = tools
        self.toolchains = toolchains.profiles(srcdir, cfg, tools)
        self.edges: list[str] = []
        self.compile_db: list[dict] = []
        self.aliases: dict[str, list[str]] = {}
        self.defaults: list[str] = []
        self.regen_inputs: set[str] = set()
        self.outputs: set[str] = set()
        self.exports: dict[str, object] = {}

    def edge(
        self,
        rule,
        outputs,
        inputs=(),
        implicit=(),
        order_only=(),
        variables=None,
        implicit_outputs=(),
    ):
        outputs = [str(o) for o in outputs]
        for o in list(outputs) + [str(o) for o in implicit_outputs]:
            if o in self.outputs:
                raise SystemExit(f"buildgen: {o} is produced by two build statements")
            self.outputs.add(o)
        line = "build " + " ".join(_esc_path(o) for o in outputs)
        if implicit_outputs:
            line += " | " + " ".join(_esc_path(str(o)) for o in implicit_outputs)
        line += ": " + rule
        if inputs:
            line += " " + " ".join(_esc_path(str(i)) for i in inputs)
        if implicit:
            line += " | " + " ".join(_esc_path(str(i)) for i in implicit)
        if order_only:
            line += " || " + " ".join(_esc_path(str(i)) for i in order_only)
        for k, v in (variables or {}).items():
            line += f"\n  {k} = {_esc_value(str(v))}"
        self.edges.append(line)

    def rel(self, path: Path | str) -> str:
        """Build-dir-relative spelling of an output path (ninja's cwd)."""
        p = Path(path)
        if p.is_absolute():
            try:
                return str(p.relative_to(self.outdir))
            except ValueError:
                return str(p)
        return str(p)


class Builder:
    """The API a BUILD.py manifest sees, scoped to the manifest's directory."""

    def __init__(self, graph: Graph, mdir: Path):
        self._g = graph
        self.dir = mdir
        self.root = graph.srcdir
        self.out = graph.outdir
        self.cfg = graph.cfg
        self.tools = graph.tools
        self.exports = graph.exports

    # paths
    def src(self, path) -> Path:
        """Absolute path of a file relative to this manifest's directory."""
        return (self.dir / path).resolve()

    def srcs(self, *paths) -> list[Path]:
        return [self.src(p) for p in paths]

    def glob(self, pattern: str, root: Path | None = None) -> list[Path]:
        """Glob at configure time. The matched directories become regeneration
        inputs, so adding or removing a file re-runs the generator."""
        base = root or self.dir
        matches = sorted(Path(p).resolve() for p in globmod.glob(str(base / pattern)))
        dirs = {str(Path(globmod.escape(str(base / pattern))).parent)}
        for d in dirs:
            if Path(d).is_dir():
                self._g.regen_inputs.add(d)
        return matches

    def obj_path(self, source: Path, suffix: str = ".o", subdir: str = "obj") -> str:
        """Object path that mirrors the source's location under the source
        tree, so equal basenames in different directories never collide."""
        try:
            rel = source.relative_to(self.root)
        except ValueError:
            rel = Path("_ext") / source.name
        return str(Path(subdir) / rel) + suffix

    # subdirectories
    def subdir(self, *dirs):
        for d in dirs:
            load_manifest(self._g, (self.dir / d).resolve())

    # toolchains
    def toolchain(self, name: str) -> "ToolchainHandle":
        tc = self._g.toolchains.get(name)
        if tc is None:
            raise SystemExit(
                f"buildgen: {self.dir}/BUILD.py: unknown toolchain {name!r}"
            )
        return ToolchainHandle(self, tc)

    # generic commands
    def command(
        self,
        outputs,
        cmd,
        inputs=(),
        implicit=(),
        order_only=(),
        cwd=None,
        description=None,
        depfile=None,
        implicit_outputs=(),
        restat=False,
    ):
        """A custom build step. `cmd` is an argv list or a list of argv lists
        (run in sequence, stopping at the first failure). `cwd` defaults to the
        build directory."""
        argvs = cmd if cmd and isinstance(cmd[0], (list, tuple)) else [cmd]
        text = " && ".join(shell_join(a) for a in argvs)
        if cwd is not None:
            text = f"cd {shlex.quote(str(cwd))} && {text}"
        outs = [self._g.rel(o) for o in outputs]
        variables = {"cmd": text, "desc": description or f"GEN {outs[0]}"}
        rule = "cmd"
        if depfile:
            variables["depfile"] = self._g.rel(depfile)
            rule = "cmd_dep"
        if restat:
            variables["restat"] = "1"
        self._g.edge(
            rule,
            outs,
            [self._g.rel(i) for i in inputs],
            [self._g.rel(i) for i in implicit],
            [self._g.rel(i) for i in order_only],
            variables,
            [self._g.rel(o) for o in implicit_outputs],
        )
        return outs

    def alias(self, name: str, *targets, default=False):
        self._g.aliases.setdefault(name, []).extend(self._g.rel(t) for t in targets)
        if default:
            self._g.defaults.append(name)

    def regen_on(self, *paths):
        for p in paths:
            self._g.regen_inputs.add(str(Path(p).resolve()))


class ToolchainHandle:
    def __init__(self, b: Builder, tc: "toolchains.Toolchain"):
        self._b = b
        self.tc = tc
        self.name = tc.name

    def compile(
        self,
        sources,
        flags=(),
        cflags=(),
        cxxflags=(),
        asflags=(),
        obj_names=None,
        subdir=None,
    ):
        """Compile each source to an object and return the object paths.

        `flags` apply to every language; `cflags`/`cxxflags`/`asflags` only to
        that language. `obj_names` maps a source to an explicit object path
        (build-dir relative) for the few outputs another step names by path.
        """
        g = self._b._g
        objs = []
        for src in sources:
            src = Path(src)
            ext = src.suffix
            if ext in (".c",):
                compiler, base, extra = self.tc.cc, self.tc.cflags, cflags
            elif ext in (".cc", ".cpp", ".cxx"):
                compiler, base, extra = self.tc.cxx, self.tc.cxxflags, cxxflags
            elif ext in (".S", ".s"):
                compiler, base, extra = self.tc.cc, self.tc.asflags, asflags
            else:
                raise SystemExit(f"buildgen: no compile rule for {src}")
            if obj_names and src in obj_names:
                obj = obj_names[src]
            else:
                obj = self._b.obj_path(src, subdir=subdir or f"obj/{self.name}")
            argv = list(base) + list(flags) + list(extra)
            # A generated source must be spelled exactly as its producing edge
            # spells its output, or ninja sees two unrelated nodes.
            g.edge(
                "cc",
                [obj],
                [g.rel(src)],
                variables={
                    "compiler": compiler,
                    "flags": shell_join(argv),
                    "tc": self.name,
                },
            )
            g.compile_db.append(
                {
                    "directory": str(g.outdir),
                    "file": str(src),
                    "output": obj,
                    "arguments": [compiler] + argv + ["-c", str(src), "-o", obj],
                }
            )
            objs.append(obj)
        return objs

    def archive(self, output, objs):
        out = self._b._g.rel(output)
        self._b._g.edge("ar", [out], objs, variables={"ar": self.tc.ar})
        return out

    def link(self, output, objs, flags=(), implicit=(), libs=()):
        out = self._b._g.rel(output)
        self._b._g.edge(
            "link",
            [out],
            objs,
            [self._b._g.rel(i) for i in implicit],
            variables={
                "linker": self.tc.ld,
                "ldflags": shell_join(list(self.tc.ldflags) + list(flags)),
                "libs": shell_join(libs),
                "tc": self.name,
            },
        )
        return out


# --- driver -----------------------------------------------------------------


def load_manifest(graph: Graph, mdir: Path):
    path = mdir / "BUILD.py"
    if not path.is_file():
        raise SystemExit(f"buildgen: {path} not found")
    graph.regen_inputs.add(str(path))
    mod = runpy.run_path(str(path))
    if "build" not in mod:
        raise SystemExit(f"buildgen: {path} defines no build(b)")
    mod["build"](Builder(graph, mdir))


RULES = """\
ninja_required_version = 1.10

rule cc
  command = $compiler -MD -MF $out.d $flags -c $in -o $out
  depfile = $out.d
  deps = gcc
  description = CC[$tc] $out

rule ar
  command = rm -f $out && $ar rcs $out $in
  description = AR $out

rule link
  command = $linker $ldflags -o $out $in $libs
  description = LINK[$tc] $out

rule cmd
  command = $cmd
  description = $desc

rule cmd_dep
  command = $cmd
  depfile = $depfile
  deps = gcc
  description = $desc

rule regen
  command = $regen_cmd
  generator = 1
  description = Regenerating build.ninja
"""


def generate(srcdir: Path, outdir: Path, tools: dict, argv_for_regen: list[str]):
    dotconfig = outdir / ".config"
    cfg = Config(srcdir / "Kconfig", dotconfig)
    graph = Graph(srcdir, outdir, cfg, tools)
    load_manifest(graph, srcdir)

    graph.regen_inputs.update(cfg.kconfig_files)
    graph.regen_inputs.add(str(dotconfig))
    graph.regen_inputs.update(str(p) for p in GENERATOR_DIR.glob("*.py"))
    graph.regen_inputs.add(str(srcdir / "configure"))

    lines = [f"# Generated by {srcdir / 'configure'} - do not edit.", RULES]
    lines.append(
        "build build.ninja: regen | "
        + " ".join(_esc_path(p) for p in sorted(graph.regen_inputs))
        + f"\n  regen_cmd = {_esc_value(shell_join(argv_for_regen))}\n"
    )
    lines.extend(e + "\n" for e in graph.edges)
    for name, targets in sorted(graph.aliases.items()):
        if name in graph.outputs:
            continue  # the file itself is already a target of that name
        lines.append(
            f"build {name}: phony " + " ".join(_esc_path(t) for t in targets) + "\n"
        )
    if graph.defaults:
        lines.append("default " + " ".join(graph.defaults) + "\n")

    _write_if_changed(outdir / "build.ninja", "\n".join(lines))
    _write_if_changed(
        outdir / "compile_commands.json", json.dumps(graph.compile_db, indent=1) + "\n"
    )
    return graph


def _write_if_changed(path: Path, text: str):
    if path.exists() and path.read_text() == text:
        # Touch so ninja sees the regeneration as done and does not loop.
        path.touch()
        return
    path.write_text(text)
