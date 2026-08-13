/* warp_aot.cpp - WASMOS ahead-of-time WARP compiler host tool.
 *
 * Reads a .wasm file, compiles it with the WARP single-pass JIT, and writes
 * the resulting native x86-64 binary blob to a .warpbin file.  The kernel can
 * then load this blob via initFromCompiledBinary() instead of JIT-compiling
 * at boot time.
 *
 * Usage: warp_aot <input.wasm> <output.warpbin>
 *
 * The symbol table uses DYNAMIC_LINK stubs whose function-pointer types match
 * the exact signatures in src/kernel/warp/link.cpp.  WARP records the type
 * signature (arity + return type) in the compiled binary; at kernel load time
 * initFromCompiledBinary() re-resolves the pointers from the kernel's live
 * symbol table.  The actual stub values here are never called. */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/NativeSymbol.hpp"
#include "src/core/common/Span.hpp"
#include "src/core/common/function_traits.hpp"
#include "src/core/common/ILogger.hpp"

/* -----------------------------------------------------------------------
 * Stub functions — one per distinct (return-type, arg-count) combination.
 * Types must exactly match the corresponding kernel hostcall signatures so
 * DYNAMIC_LINK records the correct WARP type signature in the compiled blob.
 * ----------------------------------------------------------------------- */

/* void return, 0 args  (+ void *ctx) */
static void stub_v0(void*) {}
static void stub_v1(uint32_t, void*) {}
/* void return, 4 args  (+ void *ctx) — env.abort */
static void stub_v4(uint32_t, uint32_t, uint32_t, uint32_t, void*) {}

/* uint32_t return, N args (+ void *ctx) */
static uint32_t stub_i0(void*) {
    return 0;
}
static uint32_t stub_i1(uint32_t, void*) {
    return 0;
}
static uint32_t stub_i2(uint32_t, uint32_t, void*) {
    return 0;
}
static uint32_t stub_i3(uint32_t, uint32_t, uint32_t, void*) {
    return 0;
}
static uint32_t stub_i4(uint32_t, uint32_t, uint32_t, uint32_t, void*) {
    return 0;
}
static uint32_t stub_i5(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*) {
    return 0;
}
/* 8 i32 args (ipc_send: dest, src, type, req_id, a0, a1, a2, a3, ctx) */
static uint32_t stub_i8(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                        uint32_t, void*) {
    return 0;
}

/* Symbol table generated from abi/hostcalls.yaml (scripts/gen_abi_hostcalls.py);
 * mirrors the kernel WASMOS_SYMBOLS table so initFromCompiledBinary() can rebind
 * by position at load. Stubs (stub_i<N>/stub_v<N>) are defined above. */
#include "wasmos_symbols_aot.inc"

static vb::Span<vb::NativeSymbol const> aot_symbols() {
    static vb::NativeSymbol syms[] = {WASMOS_AOT_SYMBOLS(DYNAMIC_LINK)};
    return vb::Span<vb::NativeSymbol const>(syms, sizeof(syms) / sizeof(syms[0]));
}

/* -----------------------------------------------------------------------
 * Minimal no-op logger
 * ----------------------------------------------------------------------- */

struct NullLogger final : vb::ILogger {
    NullLogger& operator<<(char const*) override {
        return *this;
    }
    NullLogger& operator<<(vb::Span<char const> const&) override {
        return *this;
    }
    NullLogger& operator<<(uint32_t) override {
        return *this;
    }
};

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: warp_aot <input.wasm> <output.warpbin>\n");
        return 1;
    }

    const char* in_path = argv[1];
    const char* out_path = argv[2];

    /* Read WASM input. */
    FILE* fin = fopen(in_path, "rb");
    if (!fin) {
        perror(in_path);
        return 1;
    }
    fseek(fin, 0, SEEK_END);
    long wsz = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (wsz <= 0) {
        fprintf(stderr, "warp_aot: empty or unreadable input: %s\n", in_path);
        fclose(fin);
        return 1;
    }
    std::vector<uint8_t> wasm((size_t)wsz);
    if (fread(wasm.data(), 1, (size_t)wsz, fin) != (size_t)wsz) {
        perror("fread");
        fclose(fin);
        return 1;
    }
    fclose(fin);

    /* WARP allocates through the hooks installed here; on the host these are the
     * C library's, not the kernel slab the in-kernel runtime installs. */
    vb::WasmModule::initEnvironment(malloc, realloc, free);

    NullLogger logger;
    /* maxRam unbounded, no debug build, no host ctx, 10 stack records -- the
     * same values vb::WasmModule's logger-only convenience constructor uses. */
    vb::WasmModule mod(UINT64_MAX, logger, false, nullptr, 10U);

    vb::Span<uint8_t const> bc(wasm.data(), wasm.size());
    try {
        /* runStart=false: do not execute the WASM start function.
         * The tool only compiles WASM to native code; it does not run it.
         * Running would fail on cross-compile hosts (e.g. arm64 host generating
         * x86-64 code) and is not needed for AOT binary extraction. */
        mod.initFromBytecode(bc, aot_symbols(), /*runStart=*/false);
    } catch (std::exception& e) {
        fprintf(stderr, "warp_aot: WARP compilation failed: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "warp_aot: WARP compilation failed (unknown exception)\n");
        return 1;
    }

    vb::Span<uint8_t const> compiled = mod.getCompiledBinary();
    if (compiled.size() == 0) {
        fprintf(stderr, "warp_aot: getCompiledBinary() returned empty span\n");
        return 1;
    }

    FILE* fout = fopen(out_path, "wb");
    if (!fout) {
        perror(out_path);
        return 1;
    }
    if (fwrite(compiled.data(), 1, compiled.size(), fout) != compiled.size()) {
        perror("fwrite");
        fclose(fout);
        return 1;
    }
    fclose(fout);

    fprintf(stderr, "warp_aot: %s -> %s (%zu bytes)\n", in_path, out_path, compiled.size());
    return 0;
}
