#include "kernel_ring3_probe_runtime.h"

#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "process.h"

extern const uint8_t _binary_ring3_native_probe_bin_start[];
extern const uint8_t _binary_ring3_native_probe_bin_end[];
extern const uint8_t _binary_ring3_thread_lifecycle_probe_bin_start[];
extern const uint8_t _binary_ring3_thread_lifecycle_probe_bin_end[];

static int
map_linear_pages(uint64_t root_table,
                 uint64_t virt_base,
                 uint64_t phys_base,
                 uint32_t size,
                 uint32_t map_flags)
{
    if (!root_table || !virt_base || !phys_base || size == 0) {
        return -1;
    }
    uint64_t page_count = (size + 0xFFFULL) / 0x1000ULL;
    for (uint64_t i = 0; i < page_count; ++i) {
        uint64_t v = virt_base + i * 0x1000ULL;
        uint64_t p = phys_base + i * 0x1000ULL;
        (void)paging_unmap_4k_in_root(root_table, v);
        if (paging_map_4k_in_root(root_table, v, p, map_flags) != 0) {
            return -1;
        }
    }
    return 0;
}

static process_run_result_t
ring3_probe_bootstrap_entry(process_t *process, void *arg)
{
    (void)arg;
    if (process) {
        process_set_exit_status(process, -1);
    }
    return PROCESS_RUN_EXITED;
}

int
kernel_ring3_spawn_native_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    process_t *proc = 0;
    mm_context_t *ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    const uint8_t *src = _binary_ring3_native_probe_bin_start;
    uint32_t code_size = (uint32_t)((uintptr_t)_binary_ring3_native_probe_bin_end -
                                    (uintptr_t)_binary_ring3_native_probe_bin_start);

    if (!out_pid || !src || code_size == 0) {
        return -1;
    }
    if (process_spawn_as(parent_pid, "ring3-native", ring3_probe_bootstrap_entry, 0, out_pid) != 0) {
        return -1;
    }
    proc = process_get(*out_pid);
    if (!proc) {
        return -1;
    }
    ctx = mm_context_get(proc->context_id);
    if (!ctx) {
        return -1;
    }
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0) {
        return -1;
    }
    if (linear.phys_base == 0 || linear.size < code_size || stack.base == 0 || stack.size < 16u) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    if (mm_copy_to_user(proc->context_id, linear.base, src, code_size) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *region = &ctx->regions[i];
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
    }
    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    klog_printf("[kernel] ring3 native pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}

int
kernel_ring3_spawn_thread_lifecycle_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    process_t *proc = 0;
    mm_context_t *ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    const uint8_t *src = _binary_ring3_thread_lifecycle_probe_bin_start;
    uint32_t code_size = (uint32_t)((uintptr_t)_binary_ring3_thread_lifecycle_probe_bin_end -
                                    (uintptr_t)_binary_ring3_thread_lifecycle_probe_bin_start);

    if (!out_pid || !src || code_size == 0) {
        return -1;
    }
    if (process_spawn_as(parent_pid, "ring3-threading", ring3_probe_bootstrap_entry, 0, out_pid) != 0) {
        return -1;
    }
    proc = process_get(*out_pid);
    if (!proc) {
        return -1;
    }
    ctx = mm_context_get(proc->context_id);
    if (!ctx) {
        return -1;
    }
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0) {
        return -1;
    }
    if (linear.phys_base == 0 || linear.size < code_size || stack.base == 0 || stack.size < 16u) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    if (mm_copy_to_user(proc->context_id, linear.base, src, code_size) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *region = &ctx->regions[i];
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
    }
    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    klog_printf("[kernel] ring3 threading pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}

static int
spawn_ring3_fault_probe_named(uint32_t parent_pid,
                              const char *name,
                              const uint8_t *code,
                              uint32_t code_size,
                              uint32_t *out_pid)
{
    process_t *proc = 0;
    mm_context_t *ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t stack_top_page_virt = 0;
    uint64_t stack_top_page_phys = 0;
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    if (!out_pid || !name || !code || code_size == 0) {
        return -1;
    }
    if (process_spawn_as(parent_pid, name, ring3_probe_bootstrap_entry, 0, out_pid) != 0) {
        return -1;
    }
    proc = process_get(*out_pid);
    if (!proc) {
        return -1;
    }
    ctx = mm_context_get(proc->context_id);
    if (!ctx) {
        return -1;
    }
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0) {
        return -1;
    }
    if (linear.phys_base == 0 || linear.size < code_size || stack.base == 0 || stack.size < 16u) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    if (mm_copy_to_user(proc->context_id, linear.base, code, code_size) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    stack_top_page_virt = (stack.base + stack.size - 1u) & ~0xFFFULL;
    stack_top_page_phys = (stack.phys_base + stack.size - 1u) & ~0xFFFULL;
    if (map_linear_pages(ctx->root_table,
                         stack_top_page_virt,
                         stack_top_page_phys,
                         0x1000u,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *region = &ctx->regions[i];
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
    }
    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    klog_printf("[kernel] %s pid=%016llx\n", name, (unsigned long long)*out_pid);
    return 0;
}

int
kernel_ring3_spawn_fault_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0xEB, 0xFE
    };
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_write_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xC7, 0x05, 0x00, 0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00, 0xEB, 0xFE
    };
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-write", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_exec_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x50, 0x48, 0x8D, 0x44, 0x24, 0x00, 0xFF, 0xE0, 0xEB, 0xFE
    };
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-exec", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_ud_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x0F, 0x0B, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-ud", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_gp_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xFA, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-gp", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_de_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x31, 0xD2, 0x31, 0xC0, 0xF7, 0xF2, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-de", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_db_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xF1, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-db", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_bp_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xCC, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-bp", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_of_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xCD, 0x04, 0x0F, 0x0B, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-of", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_nm_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xD9, 0xE8, 0x0F, 0x0B, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-nm", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_ss_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x48, 0xBC, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x50, 0x0F, 0x0B, 0xEB, 0xFE
    };
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-ss", code, (uint32_t)sizeof(code), out_pid);
}

int
kernel_ring3_spawn_fault_ac_probe(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x9C, 0x58, 0x48, 0x0D, 0x00, 0x00, 0x04, 0x00,
        0x50, 0x9D, 0x48, 0x8D, 0x44, 0x24, 0x01, 0x8B, 0x00, 0x0F, 0x0B, 0xEB, 0xFE
    };
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-ac", code, (uint32_t)sizeof(code), out_pid);
}
