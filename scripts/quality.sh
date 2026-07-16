#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

mode="${1:-}"
shift || true

check_mode=0
build_dir="${QUALITY_BUILD_DIR:-build}"

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

collect_files() {
    local file
    while IFS= read -r -d '' file; do
        case "$file" in
            libs/*|build/*|build-*/*|userfs/*|node_modules/*)
                continue
                ;;
            *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp)
                c_format_files+=("$file")
                case "$file" in
                    *.c|*.cc|*.cpp|*.cxx)
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
    local clang_format
    clang_format="$(require_tool CLANG_FORMAT \
        "clang-format is required. Set CLANG_FORMAT or install LLVM clang-format." \
        clang-format /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format)"

    local -a format_targets=("${c_format_files[@]}" "${as_files[@]}")
    if [[ ${#format_targets[@]} -eq 0 ]]; then
        return 0
    fi

    if [[ "$check_mode" -eq 1 ]]; then
        "$clang_format" --style=file --dry-run --Werror "${format_targets[@]}"
    else
        "$clang_format" --style=file -i "${format_targets[@]}"
    fi
}

run_gofmt() {
    local gofmt
    gofmt="$(require_tool GOFMT "gofmt is required. Set GOFMT or install Go." gofmt /usr/local/go/bin/gofmt)"
    if [[ ${#go_files[@]} -eq 0 ]]; then
        return 0
    fi

    if [[ "$check_mode" -eq 1 ]]; then
        local diff_output
        diff_output="$("$gofmt" -d "${go_files[@]}")"
        if [[ -n "$diff_output" ]]; then
            printf '%s\n' "$diff_output"
            echo "error: gofmt reported formatting changes" >&2
            exit 1
        fi
    else
        "$gofmt" -w "${go_files[@]}"
    fi
}

run_zig_fmt() {
    local zig
    zig="$(require_tool ZIG "zig is required. Set ZIG or install Zig." zig)"
    if [[ ${#zig_files[@]} -eq 0 ]]; then
        return 0
    fi

    if [[ "$check_mode" -eq 1 ]]; then
        "$zig" fmt --check "${zig_files[@]}"
    else
        "$zig" fmt "${zig_files[@]}"
    fi
}

run_rustfmt() {
    local rustfmt
    rustfmt="$(require_tool RUSTFMT "rustfmt is required. Set RUSTFMT or install Rustfmt." rustfmt)"
    if [[ ${#rust_format_files[@]} -eq 0 ]]; then
        return 0
    fi

    if [[ "$check_mode" -eq 1 ]]; then
        "$rustfmt" --edition 2021 --check "${rust_format_files[@]}"
    else
        "$rustfmt" --edition 2021 "${rust_format_files[@]}"
    fi
}

run_clang_tidy() {
    local clang_tidy
    clang_tidy="$(require_tool CLANG_TIDY \
        "clang-tidy is required. Set CLANG_TIDY or install LLVM clang-tidy." \
        clang-tidy /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy)"

    if [[ ${#c_tidy_files[@]} -eq 0 ]]; then
        return 0
    fi

    if [[ ! -f "$build_dir/compile_commands.json" ]]; then
        echo "error: $build_dir/compile_commands.json is missing. Re-run cmake -S . -B $build_dir before make lint." >&2
        exit 1
    fi

    "$clang_tidy" -p "$build_dir" --quiet "${c_tidy_files[@]}"
}

run_go_lint() {
    local go
    go="$(require_tool GO "go is required. Set GO or install Go." go /usr/local/go/bin/go)"

    if [[ ! -f "examples/go/hello/hello_go.go" ]]; then
        return 0
    fi

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
    local zig
    zig="$(require_tool ZIG "zig is required. Set ZIG or install Zig." zig)"
    if [[ ${#zig_files[@]} -eq 0 ]]; then
        return 0
    fi
    "$zig" ast-check "${zig_files[@]}"
}

run_rust_lint() {
    local rustc
    rustc="$(require_tool RUSTC "rustc is required. Set RUSTC or install Rust." rustc)"
    if [[ ${#rust_lint_files[@]} -eq 0 ]]; then
        return 0
    fi

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
    local asc
    asc="$(require_tool ASC \
        "AssemblyScript compiler 'asc' is required. Run npm install or set ASC." \
        "$repo_root/node_modules/.bin/asc" asc)"

    if [[ ${#as_files[@]} -eq 0 ]]; then
        return 0
    fi

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
            cat > "$stage_dir/app.ts" <<'EOF'
export function main(_args: string[] | null): i32 {
    return 0;
}
EOF
        fi

        "$asc" "$entry_file" --target release -Osize --runtime stub --noAssert --outFile "$out_file"
        rm -rf "$stage_dir"
    done
}

case "$mode" in
    format)
        run_clang_format
        run_gofmt
        run_zig_fmt
        run_rustfmt
        ;;
    lint)
        run_clang_tidy
        run_go_lint
        run_zig_lint
        run_rust_lint
        run_assemblyscript_lint
        ;;
    *)
        echo "error: unknown mode: $mode" >&2
        usage >&2
        exit 1
        ;;
esac
