#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

mode="${1:-}"
shift || true

check_mode=0
build_dir="${QUALITY_BUILD_DIR:-build}"

# Allowlist of top-level directories that hold first-party source. Only files
# under these roots are formatted/linted; everything else is excluded by
# omission: vendored subtrees (libs/), others/, dot folders (.claude/, .github
# is intentionally not listed), build trees, node_modules, and top-level files.
# Edit this list to widen or narrow the scope.
allowed_roots=(src tests examples scripts)

usage() {
    cat <<'EOF'
Usage:
  ./scripts/quality.sh format [--check]
  ./scripts/quality.sh lint

Environment overrides:
  QUALITY_BUILD_DIR   Build directory used for clang-tidy (default: build)
  CLANG_FORMAT        Path to clang-format
  CLANG_TIDY          Path to clang-tidy
  GO                  Path to go
  GOFMT               Path to gofmt
  ZIG                 Path to zig
  RUSTFMT             Path to rustfmt
  RUSTC               Path to rustc
  ASC                 Path to the AssemblyScript compiler
  BLACK               Path to black (Python formatter)
  RUFF                Path to ruff (Python linter)
EOF
}

if [[ -z "$mode" ]]; then
    usage
    exit 1
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check)
            check_mode=1
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
    shift
done

declare -a c_format_files=()
declare -a c_tidy_files=()
declare -a go_files=()
declare -a zig_files=()
declare -a rust_format_files=()
declare -a rust_lint_files=()
declare -a as_files=()
declare -a py_files=()

# Announce the start of a per-language step (only emitted when it has work).
step() {
    printf '==> %s\n' "$*"
}

find_tool() {
    local env_name="$1"
    shift
    local candidates=()
    local override="${!env_name:-}"
    if [[ -n "$override" ]]; then
        candidates+=("$override")
    fi
    candidates+=("$@")

    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"
            return 0
        fi
    done
    return 1
}

require_tool() {
    local env_name="$1"
    local help_message="$2"
    shift 2
    local tool
    if ! tool="$(find_tool "$env_name" "$@")"; then
        echo "error: ${help_message}" >&2
        exit 1
    fi
    printf '%s\n' "$tool"
}

# True if $1 lives under one of the allowed top-level roots.
is_allowed_path() {
    local top="${1%%/*}"
    local root
    [[ "$top" == "$1" ]] && return 1 # top-level file (no directory): excluded
    for root in "${allowed_roots[@]}"; do
        [[ "$top" == "$root" ]] && return 0
    done
    return 1
}

collect_files() {
    local file
    while IFS= read -r -d '' file; do
        is_allowed_path "$file" || continue
        case "$file" in
            *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp)
                c_format_files+=("$file")
                # clang-tidy is scoped to C only. The freestanding C++ TUs
                # (kernel/warp glue, warp_aot host tools) compile with -std=gnu++17
                # against libc++ headers but no matching hosted C library, so
                # clang-tidy's standalone parse chokes on libc++ <wchar.h> even
                # though the real build compiles them. clang-format still covers
                # C++ above. TODO: add the right --extra-arg flags to lint C++.
                case "$file" in
                    *.c)
                        c_tidy_files+=("$file")
                        ;;
                esac
                ;;
            *.go)
                go_files+=("$file")
                ;;
            *.zig)
                zig_files+=("$file")
                ;;
            *.py)
                py_files+=("$file")
                ;;
            *.rs)
                rust_format_files+=("$file")
                case "$file" in
                    examples/rust/*)
                        rust_lint_files+=("$file")
                        ;;
                esac
                ;;
            examples/assemblyscript/*.ts|examples/assemblyscript/**/*.ts|src/drivers/*.ts|src/drivers/**/*.ts|src/utils/*.ts|src/utils/**/*.ts|src/libc/assemblyscript/*.ts|src/libui/assemblyscript/*.ts)
                as_files+=("$file")
                ;;
        esac
    done < <(git ls-files -z)
}

collect_files

run_clang_format() {
    local -a format_targets=("${c_format_files[@]}" "${as_files[@]}")
    if [[ ${#format_targets[@]} -eq 0 ]]; then
        return 0
    fi
    local clang_format
    clang_format="$(require_tool CLANG_FORMAT \
        "clang-format is required. Set CLANG_FORMAT or install LLVM clang-format." \
        clang-format /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format)"

    if [[ "$check_mode" -eq 1 ]]; then
        step "Checking C/C++ and AssemblyScript formatting (clang-format)..."
        "$clang_format" --style=file --dry-run --Werror "${format_targets[@]}"
    else
        step "Formatting C/C++ and AssemblyScript sources (clang-format)..."
        "$clang_format" --style=file -i "${format_targets[@]}"
    fi
}

run_gofmt() {
    if [[ ${#go_files[@]} -eq 0 ]]; then
        return 0
    fi
    local gofmt
    gofmt="$(require_tool GOFMT "gofmt is required. Set GOFMT or install Go." gofmt /usr/local/go/bin/gofmt)"

    if [[ "$check_mode" -eq 1 ]]; then
        step "Checking Go formatting (gofmt)..."
        local diff_output
        diff_output="$("$gofmt" -d "${go_files[@]}")"
        if [[ -n "$diff_output" ]]; then
            printf '%s\n' "$diff_output"
            echo "error: gofmt reported formatting changes" >&2
            exit 1
        fi
    else
        step "Formatting Go sources (gofmt)..."
        "$gofmt" -w "${go_files[@]}"
    fi
}

run_zig_fmt() {
    if [[ ${#zig_files[@]} -eq 0 ]]; then
        return 0
    fi
    local zig
    zig="$(require_tool ZIG "zig is required. Set ZIG or install Zig." zig)"

    if [[ "$check_mode" -eq 1 ]]; then
        step "Checking Zig formatting (zig fmt)..."
        "$zig" fmt --check "${zig_files[@]}"
    else
        step "Formatting Zig sources (zig fmt)..."
        "$zig" fmt "${zig_files[@]}"
    fi
}

run_rustfmt() {
    if [[ ${#rust_format_files[@]} -eq 0 ]]; then
        return 0
    fi
    local rustfmt
    rustfmt="$(require_tool RUSTFMT "rustfmt is required. Set RUSTFMT or install Rustfmt." rustfmt)"

    if [[ "$check_mode" -eq 1 ]]; then
        step "Checking Rust formatting (rustfmt)..."
        "$rustfmt" --edition 2021 --check "${rust_format_files[@]}"
    else
        step "Formatting Rust sources (rustfmt)..."
        "$rustfmt" --edition 2021 "${rust_format_files[@]}"
    fi
}

run_python_format() {
    if [[ ${#py_files[@]} -eq 0 ]]; then
        return 0
    fi
    local black
    black="$(require_tool BLACK \
        "black is required for Python formatting. Set BLACK or 'pip install black'." \
        black)"

    if [[ "$check_mode" -eq 1 ]]; then
        step "Checking Python formatting (black)..."
        "$black" --quiet --check "${py_files[@]}"
    else
        step "Formatting Python sources (black)..."
        "$black" --quiet "${py_files[@]}"
    fi
}

run_clang_tidy() {
    if [[ ${#c_tidy_files[@]} -eq 0 ]]; then
        return 0
    fi
    local clang_tidy
    clang_tidy="$(require_tool CLANG_TIDY \
        "clang-tidy is required. Set CLANG_TIDY or install LLVM clang-tidy." \
        clang-tidy /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy)"

    if [[ ! -f "$build_dir/compile_commands.json" ]]; then
        echo "error: $build_dir/compile_commands.json is missing. Re-run cmake -S . -B $build_dir before lint." >&2
        exit 1
    fi

    step "Linting C sources (clang-tidy)..."

    # The compile DB is populated by the *_ide helper targets, which compile
    # every source for the host arch ("-arch arm64" on Apple Silicon) with no
    # cross target. Replaying those flags makes clang-tidy analyze the x86_64
    # kernel (and its inline asm) as arm64 and reject it, and it also conflicts
    # with any injected --target ("-arch" is unsupported off Darwin). So write a
    # sanitized DB into a temp dir: drop "-arch <x>" and inject the real per-file
    # target derived from the component's linker.metadata. native=true (kernel-
    # native services/drivers) and sources with no linker.metadata (kernel, boot,
    # tools) are x86_64; everything else (WASM userland) is wasm32.
    local tidy_db
    tidy_db="$(mktemp -d "${TMPDIR:-/tmp}/wasmos-tidy-db.XXXXXX")"
    trap 'rm -rf "$tidy_db"' RETURN

    python3 - "$build_dir/compile_commands.json" "$tidy_db/compile_commands.json" "$repo_root" <<'PY'
import json, os, sys

src_db, out_db, root = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])

# Host build tools need a hosted libc, but the *_ide targets compile every
# source freestanding (-nostdinc), so their DB commands can't resolve <stdio.h>
# etc. They are build-time host tooling with little lint value, so exclude them
# from clang-tidy entirely (dropped from the sanitized DB below). posix_kernel.c
# is a POSIX host shim that happens to live under src/kernel/warp; only the build
# system knows it is host-side, hence the explicit entry.
HOST_TOOLS = ("scripts/", "src/tools/", "src/kernel/warp/posix_kernel.c")

def decide(path):
    """Return (kind, extra_flags). kind: None=skip, 'wasm', or 'x86'."""
    rel = os.path.relpath(os.path.abspath(path), root)
    if rel.startswith(HOST_TOOLS):
        return None, []
    if rel.startswith("src/boot/"):
        # UEFI: 16-bit wchar_t, so L"" literals need -fshort-wchar to parse.
        return "x86", ["-fshort-wchar"]
    d = os.path.dirname(os.path.abspath(path))
    while True:
        meta = os.path.join(d, "linker.metadata")
        if os.path.isfile(meta):
            native = False
            for line in open(meta):
                s = line.split("#", 1)[0].replace(" ", "").strip()
                if s.startswith("native=true"):
                    native = True
                    break
                if s.startswith("native=false"):
                    native = False
                    break
            return ("x86" if native else "wasm"), []
        if d == root or d == os.path.dirname(d):
            # No component metadata up to the repo root: kernel core.
            return "x86", []
        d = os.path.dirname(d)

out = []
for e in json.load(open(src_db)):
    kind, extra = decide(e["file"])
    if kind is None:
        continue  # host tool: excluded from linting (see HOST_TOOLS)
    cmd = e.get("command") or " ".join(e.get("arguments", []))
    toks = cmd.split()
    had_arch = "-arch" in toks
    res, i = [], 0
    while i < len(toks):
        if toks[i] == "-arch":  # strip host-arch selector (Apple driver)
            i += 2
            continue
        res.append(toks[i])
        i += 1
    if kind == "wasm":
        # WASM userland uses __builtin_wasm_*; the *_ide targets compile it for
        # the host, so force the wasm32 target on every platform.
        res[1:1] = ["--target=wasm32-unknown-unknown"]
    else:
        # Retarget only when the IDE command selected the host arch via -arch
        # (macOS: arm64), which can't parse x86_64 inline asm. Where the command
        # already targets the host correctly (no -arch, e.g. Linux x86_64),
        # leave the target alone so host header resolution keeps working (a bare
        # -none-elf there breaks <stdint.h>). But extra flags (e.g. -fshort-wchar
        # for boot's CHAR16 literals) are needed to parse on every platform, so
        # always apply them.
        inject = (["--target=x86_64-unknown-none-elf"] if had_arch else []) + extra
        if inject:
            res[1:1] = inject
    e["command"] = " ".join(res)
    e.pop("arguments", None)
    out.append(e)
json.dump(out, open(out_db, "w"))
PY

    # clang-tidy can only analyze files that have an entry in the compile DB
    # (the *_ide targets cover kernel/driver/service sources but not, e.g., the
    # host unit tests or the WASM libc). Skip the rest silently.
    local -a tidy=()
    local f
    for f in "${c_tidy_files[@]}"; do
        if grep -Fq "\"$repo_root/$f\"" "$tidy_db/compile_commands.json"; then
            tidy+=("$f")
        fi
    done
    if [[ ${#tidy[@]} -eq 0 ]]; then
        return 0
    fi

    # Load the wasmos clang-tidy plugin (built by CMake) so the wasmos-* checks
    # run; without it clang-tidy just skips those checks.
    local -a load_args=()
    if [[ -n "${QUALITY_TIDY_PLUGIN:-}" && -f "$QUALITY_TIDY_PLUGIN" ]]; then
        load_args+=(--load "$QUALITY_TIDY_PLUGIN")
    fi

    "$clang_tidy" "${load_args[@]}" -p "$tidy_db" --quiet "${tidy[@]}"
}

run_go_lint() {
    if [[ ! -f "examples/go/hello/hello_go.go" ]]; then
        return 0
    fi
    local go
    go="$(require_tool GO "go is required. Set GO or install Go." go /usr/local/go/bin/go)"

    step "Linting Go sources (go vet)..."
    local stage_dir
    stage_dir="$(mktemp -d "${TMPDIR:-/tmp}/wasmos-go-lint.XXXXXX")"
    trap 'rm -rf "$stage_dir"' RETURN

    cp "examples/go/hello/hello_go.go" "$stage_dir/hello_go.go"
    cp "src/libc/go/wasmos.go" "$stage_dir/wasmos.go"

    (
        cd "$stage_dir"
        GO111MODULE=off "$go" vet .
    )
}

run_zig_lint() {
    if [[ ${#zig_files[@]} -eq 0 ]]; then
        return 0
    fi
    local zig
    zig="$(require_tool ZIG "zig is required. Set ZIG or install Zig." zig)"
    step "Linting Zig sources (zig ast-check)..."
    # zig ast-check accepts a single file per invocation.
    local f
    for f in "${zig_files[@]}"; do
        "$zig" ast-check "$f"
    done
}

run_rust_lint() {
    if [[ ${#rust_lint_files[@]} -eq 0 ]]; then
        return 0
    fi
    local rustc
    rustc="$(require_tool RUSTC "rustc is required. Set RUSTC or install Rust." rustc)"

    step "Linting Rust sources (rustc -Dwarnings)..."
    local file
    for file in "${rust_lint_files[@]}"; do
        local out_file
        out_file="$(mktemp "${TMPDIR:-/tmp}/wasmos-rust-lint.XXXXXX.rmeta")"
        "$rustc" \
            --edition=2021 \
            --emit metadata \
            --crate-type cdylib \
            --target wasm32-unknown-unknown \
            -Dwarnings \
            "$file" \
            -o "$out_file"
        rm -f "$out_file"
    done
}

copy_if_needed() {
    local source_file="$1"
    local dest_file="$2"
    if [[ -f "$source_file" ]]; then
        cp "$source_file" "$dest_file"
    fi
}

run_assemblyscript_lint() {
    if [[ ${#as_files[@]} -eq 0 ]]; then
        return 0
    fi
    local asc
    asc="$(require_tool ASC \
        "AssemblyScript compiler 'asc' is required. Run npm install or set ASC." \
        "$repo_root/node_modules/.bin/asc" asc)"

    step "Linting AssemblyScript sources (asc)..."
    local file
    for file in "${as_files[@]}"; do
        local stage_dir entry_file out_file
        stage_dir="$(mktemp -d "${TMPDIR:-/tmp}/wasmos-asc-lint.XXXXXX")"
        entry_file="$stage_dir/$(basename "$file")"
        out_file="$stage_dir/out.wasm"

        cp "$file" "$entry_file"

        if grep -q '"\./wasmos"' "$file" || [[ "$file" == "src/libui/assemblyscript/libui.ts" || "$file" == "src/libc/assemblyscript/runtime.ts" ]]; then
            copy_if_needed "src/libc/assemblyscript/wasmos.ts" "$stage_dir/wasmos.ts"
        fi

        if grep -q '"\./libui"' "$file"; then
            copy_if_needed "src/libui/assemblyscript/libui.ts" "$stage_dir/libui.ts"
            copy_if_needed "src/libc/assemblyscript/wasmos.ts" "$stage_dir/wasmos.ts"
        fi

        if [[ "$file" == "src/libc/assemblyscript/runtime.ts" ]]; then
            # runtime.ts imports main from ./app and hands it to runMain, whose
            # parameter type is (Array<string>) => i32. Match that signature (and
            # the real apps') or asc rejects the stub with TS2322.
            cat > "$stage_dir/app.ts" <<'EOF'
export function main(_args: Array<string>): i32 {
    return 0;
}
EOF
        fi

        "$asc" "$entry_file" --target release -Osize --runtime stub --noAssert --outFile "$out_file"
        rm -rf "$stage_dir"
    done
}

run_python_lint() {
    if [[ ${#py_files[@]} -eq 0 ]]; then
        return 0
    fi
    local ruff
    ruff="$(require_tool RUFF \
        "ruff is required for Python linting. Set RUFF or 'pip install ruff'." \
        ruff)"
    step "Linting Python sources (ruff)..."
    "$ruff" check "${py_files[@]}"
}

case "$mode" in
    format)
        run_clang_format
        run_gofmt
        run_zig_fmt
        run_rustfmt
        run_python_format
        ;;
    lint)
        run_clang_tidy
        run_go_lint
        run_zig_lint
        run_rust_lint
        run_assemblyscript_lint
        run_python_lint
        ;;
    *)
        echo "error: unknown mode: $mode" >&2
        usage >&2
        exit 1
        ;;
esac
