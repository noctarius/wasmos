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
static void     stub_v0(void *) {}
/* void return, 4 args  (+ void *ctx) — env.abort */
static void     stub_v4(uint32_t, uint32_t, uint32_t, uint32_t, void *) {}

/* uint32_t return, N args (+ void *ctx) */
static uint32_t stub_i0(void *)                                                          { return 0; }
static uint32_t stub_i1(uint32_t, void *)                                                { return 0; }
static uint32_t stub_i2(uint32_t, uint32_t, void *)                                     { return 0; }
static uint32_t stub_i3(uint32_t, uint32_t, uint32_t, void *)                           { return 0; }
static uint32_t stub_i4(uint32_t, uint32_t, uint32_t, uint32_t, void *)                 { return 0; }
static uint32_t stub_i5(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void *)       { return 0; }
/* 8 i32 args (ipc_send: dest, src, type, req_id, a0, a1, a2, a3, ctx) */
static uint32_t stub_i8(uint32_t, uint32_t, uint32_t, uint32_t,
                        uint32_t, uint32_t, uint32_t, uint32_t, void *)                  { return 0; }

/* -----------------------------------------------------------------------
 * Symbol table — mirrors the DYNAMIC_LINK table in src/kernel/warp/link.cpp.
 * Every entry here must have the same type signature as the kernel version so
 * that initFromCompiledBinary() can rebind function pointers at load time.
 * ----------------------------------------------------------------------- */

static vb::Span<vb::NativeSymbol const> aot_symbols()
{
    static vb::NativeSymbol syms[] = {
        /* IPC */
        DYNAMIC_LINK("wasmos", "ipc_create_endpoint", stub_i0),
        DYNAMIC_LINK("wasmos", "ipc_endpoint_owner",  stub_i1),
        /* ipc_send: dest, src, type, req_id, a0, a1, a2, a3 */
        DYNAMIC_LINK("wasmos", "ipc_send",            stub_i8),
        DYNAMIC_LINK("wasmos", "ipc_select_one",      stub_i1),
        DYNAMIC_LINK("wasmos", "ipc_recv",            stub_i1),  /* alias */
        DYNAMIC_LINK("wasmos", "ipc_drain",           stub_i1),
        DYNAMIC_LINK("wasmos", "ipc_try_recv",        stub_i1),  /* alias */
        DYNAMIC_LINK("wasmos", "ipc_notify",          stub_i1),
        DYNAMIC_LINK("wasmos", "ipc_last_field",      stub_i1),
        /* IPC select sets */
        DYNAMIC_LINK("wasmos", "ipc_select_create",   stub_i0),
        DYNAMIC_LINK("wasmos", "ipc_select_add",      stub_i2),
        DYNAMIC_LINK("wasmos", "ipc_select_wait",     stub_i1),
        DYNAMIC_LINK("wasmos", "ipc_select_destroy",  stub_i1),
        DYNAMIC_LINK("wasmos", "sys_select_create",   stub_i0),
        DYNAMIC_LINK("wasmos", "sys_select_add",      stub_i2),
        DYNAMIC_LINK("wasmos", "sys_select_wait",     stub_i1),
        DYNAMIC_LINK("wasmos", "sys_select_destroy",  stub_i1),
        /* Console */
        DYNAMIC_LINK("wasmos", "console_read",        stub_i2),
        DYNAMIC_LINK("wasmos", "console_write",       stub_i2),
        /* Process / scheduler */
        DYNAMIC_LINK("wasmos", "proc_exit",           stub_i1),
        DYNAMIC_LINK("wasmos", "proc_notify_ready",   stub_i0),
        DYNAMIC_LINK("wasmos", "sched_yield",         stub_i0),
        DYNAMIC_LINK("wasmos", "sched_current_pid",   stub_i0),
        DYNAMIC_LINK("wasmos", "thread_gettid",       stub_i0),
        /* Futex */
        DYNAMIC_LINK("wasmos", "futex_wait",          stub_i3),
        DYNAMIC_LINK("wasmos", "futex_wake",          stub_i2),
        /* FS shared buffer */
        DYNAMIC_LINK("wasmos", "fs_buffer_size",      stub_i0),
        DYNAMIC_LINK("wasmos", "fs_endpoint",         stub_i0),
        DYNAMIC_LINK("wasmos", "fs_buffer_copy",      stub_i3),
        DYNAMIC_LINK("wasmos", "fs_buffer_write",     stub_i3),
        DYNAMIC_LINK("wasmos", "fs_buffer_borrow",    stub_i2),
        DYNAMIC_LINK("wasmos", "fs_buffer_release",   stub_i0),
        /* Generic buffer borrow/release */
        DYNAMIC_LINK("wasmos", "buffer_borrow",       stub_i3),
        DYNAMIC_LINK("wasmos", "buffer_release",      stub_i1),
        /* Block DMA buffer */
        DYNAMIC_LINK("wasmos", "block_buffer_phys",   stub_i0),
        DYNAMIC_LINK("wasmos", "block_buffer_copy",   stub_i4),
        DYNAMIC_LINK("wasmos", "block_buffer_write",  stub_i4),
        /* I/O ports */
        DYNAMIC_LINK("wasmos", "io_in8",              stub_i1),
        DYNAMIC_LINK("wasmos", "io_in16",             stub_i1),
        DYNAMIC_LINK("wasmos", "io_in32",             stub_i1),
        DYNAMIC_LINK("wasmos", "io_out8",             stub_i2),
        DYNAMIC_LINK("wasmos", "io_out16",            stub_i2),
        DYNAMIC_LINK("wasmos", "io_out32",            stub_i2),
        DYNAMIC_LINK("wasmos", "io_wait",             stub_i0),
        /* ACPI / boot info */
        DYNAMIC_LINK("wasmos", "acpi_rsdp_info",      stub_i3),
        DYNAMIC_LINK("wasmos", "boot_module_name",    stub_i3),
        DYNAMIC_LINK("wasmos", "sync_user_read",      stub_i2),
        /* System */
        DYNAMIC_LINK("wasmos", "system_halt",         stub_i0),
        DYNAMIC_LINK("wasmos", "system_reboot",       stub_i0),
        /* Scheduler extras */
        DYNAMIC_LINK("wasmos", "sched_ticks",         stub_i0),
        DYNAMIC_LINK("wasmos", "proc_count",          stub_i0),
        DYNAMIC_LINK("wasmos", "sched_ready_count",   stub_i0),
        DYNAMIC_LINK("wasmos", "sched_cpu_count",     stub_i0),
        DYNAMIC_LINK("wasmos", "sched_cpu_stats",     stub_i2),
        DYNAMIC_LINK("wasmos", "physmem_stats",       stub_i1),  /* (out_off, ctx) */
        DYNAMIC_LINK("wasmos", "kernel_runtime",      stub_i0),
        DYNAMIC_LINK("wasmos", "debug_mark",          stub_i1),
        DYNAMIC_LINK("wasmos", "kmap_dump",           stub_i0),
        DYNAMIC_LINK("wasmos", "kmap_dump_all",       stub_i0),
        /* initfs */
        DYNAMIC_LINK("wasmos", "initfs_entry_count",  stub_i0),
        DYNAMIC_LINK("wasmos", "initfs_entry_name",   stub_i3),
        DYNAMIC_LINK("wasmos", "initfs_entry_size",   stub_i1),
        DYNAMIC_LINK("wasmos", "initfs_entry_copy",   stub_i4),
        DYNAMIC_LINK("wasmos", "initfs_find_path",    stub_i2),
        /* DMA */
        DYNAMIC_LINK("wasmos", "dma_map_borrow",      stub_i5),
        DYNAMIC_LINK("wasmos", "dma_sync_borrow",     stub_i4),
        DYNAMIC_LINK("wasmos", "dma_unmap_borrow",    stub_i2),
        /* Physical memory */
        DYNAMIC_LINK("wasmos", "phys_map",            stub_i4),
        /* Process info */
        DYNAMIC_LINK("wasmos", "proc_info",           stub_i3),
        DYNAMIC_LINK("wasmos", "proc_info_ex",        stub_i4),
        DYNAMIC_LINK("wasmos", "proc_info_stats",     stub_i5),
        /* Threads */
        DYNAMIC_LINK("wasmos", "thread_create",       stub_i4),
        DYNAMIC_LINK("wasmos", "thread_yield",        stub_i0),
        DYNAMIC_LINK("wasmos", "thread_exit",         stub_i1),
        DYNAMIC_LINK("wasmos", "thread_join",         stub_i1),
        DYNAMIC_LINK("wasmos", "thread_detach",       stub_i1),
        /* Shared memory */
        DYNAMIC_LINK("wasmos", "shmem_create",        stub_i2),
        DYNAMIC_LINK("wasmos", "shmem_grant",         stub_i2),
        DYNAMIC_LINK("wasmos", "shmem_revoke",        stub_i2),
        DYNAMIC_LINK("wasmos", "shmem_map",           stub_i3),
        DYNAMIC_LINK("wasmos", "shmem_map_auto",      stub_i2),
        DYNAMIC_LINK("wasmos", "shmem_flush",         stub_i3),
        DYNAMIC_LINK("wasmos", "shmem_refresh",       stub_i3),
        DYNAMIC_LINK("wasmos", "shmem_unmap",         stub_i1),
        /* IRQ */
        DYNAMIC_LINK("wasmos", "irq_route_ipc",       stub_i2),
        DYNAMIC_LINK("wasmos", "irq_ack",             stub_i1),
        DYNAMIC_LINK("wasmos", "irq_unroute",         stub_i1),
        /* Serial / input */
        DYNAMIC_LINK("wasmos", "serial_register",     stub_i1),
        DYNAMIC_LINK("wasmos", "input_push",          stub_i1),
        DYNAMIC_LINK("wasmos", "input_read",          stub_i0),
        /* Framebuffer */
        DYNAMIC_LINK("wasmos", "framebuffer_info",    stub_i2),
        DYNAMIC_LINK("wasmos", "framebuffer_map",     stub_i2),
        DYNAMIC_LINK("wasmos", "framebuffer_pixel",   stub_i3),
        /* Boot config */
        DYNAMIC_LINK("wasmos", "boot_config_size",    stub_i0),
        DYNAMIC_LINK("wasmos", "boot_config_copy",    stub_i3),
        /* Early log */
        DYNAMIC_LINK("wasmos", "early_log_size",      stub_i0),
        DYNAMIC_LINK("wasmos", "early_log_copy",      stub_i3),
        /* Environment */
        DYNAMIC_LINK("wasmos", "env_get",             stub_i4),
        DYNAMIC_LINK("wasmos", "env_set",             stub_i4),
        DYNAMIC_LINK("wasmos", "env_unset",           stub_i2),
        /* AssemblyScript runtime */
        DYNAMIC_LINK("env",    "abort",               stub_v4),
    };
    return vb::Span<vb::NativeSymbol const>(syms, sizeof(syms) / sizeof(syms[0]));
}

/* -----------------------------------------------------------------------
 * Minimal no-op logger
 * ----------------------------------------------------------------------- */

struct NullLogger final : vb::ILogger {
    NullLogger &operator<<(char const *) override { return *this; }
    NullLogger &operator<<(vb::Span<char const> const &) override { return *this; }
    NullLogger &operator<<(uint32_t) override { return *this; }
};

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: warp_aot <input.wasm> <output.warpbin>\n");
        return 1;
    }

    const char *in_path  = argv[1];
    const char *out_path = argv[2];

    /* Read WASM input. */
    FILE *fin = fopen(in_path, "rb");
    if (!fin) { perror(in_path); return 1; }
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

    /* Initialise WARP allocator (malloc/realloc/free from host_shim.cpp path). */
    vb::WasmModule::initEnvironment(malloc, realloc, free);

    NullLogger logger;
    vb::WasmModule mod(UINT64_MAX, logger, false, nullptr, 10U);

    vb::Span<uint8_t const> bc(wasm.data(), wasm.size());
    try {
        /* runStart=false: do not execute the WASM start function.
         * The tool only compiles WASM to native code; it does not run it.
         * Running would fail on cross-compile hosts (e.g. arm64 host generating
         * x86-64 code) and is not needed for AOT binary extraction. */
        mod.initFromBytecode(bc, aot_symbols(), /*runStart=*/false);
    } catch (std::exception &e) {
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

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { perror(out_path); return 1; }
    if (fwrite(compiled.data(), 1, compiled.size(), fout) != compiled.size()) {
        perror("fwrite");
        fclose(fout);
        return 1;
    }
    fclose(fout);

    fprintf(stderr, "warp_aot: %s -> %s (%zu bytes)\n",
            in_path, out_path, compiled.size());
    return 0;
}
