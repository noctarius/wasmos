/* kernel_ring3_probe_runtime.c - Ring-3 probe launcher.
 *
 * Loads the two flat probe binaries (ring3_native_probe.bin,
 * ring3_thread_lifecycle_probe.bin) and the twelve inline fault probes below
 * into ring-3 processes. Each spawn follows the same shape: park the process,
 * map its linear region read-write to copy the code in, remap it read-execute,
 * mirror that on the region flags so a later fault is judged against R+X,
 * remap the top stack page user-writable, set the user entry, then unpark.
 *
 * Nothing here checks a probe's outcome: these functions return 0 once the
 * process is running. The verdict for the fault probes is reached by
 * kernel_ring3_fault_runtime.c from their exit statuses. */
#include "kernel_ring3_probe_runtime.h"

#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "process.h"

extern const uint8_t _binary_ring3_native_probe_bin_start[];
extern const uint8_t _binary_ring3_native_probe_bin_end[];
extern const uint8_t _binary_ring3_thread_lifecycle_probe_bin_start[];
extern const uint8_t _binary_ring3_thread_lifecycle_probe_bin_end[];

static int map_linear_pages(uint64_t root_table, uint64_t virt_base, uint64_t phys_base,
                            uint32_t size, uint32_t map_flags) {
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

static process_run_result_t ring3_probe_bootstrap_entry(process_t* process, void* arg) {
    (void)arg;
    if (process) {
        process_set_exit_status(process, -1);
    }
    return PROCESS_RUN_EXITED;
}

/* Spawns the flat ring3_native_probe.bin as a ring-3 child of parent_pid.
 *
 * The process is created PARKED and only unparked after the code is copied in,
 * the linear region is remapped read-execute (and its region flags updated to
 * match, so a later fault is judged against R+X), the top stack page is made
 * user-writable, and the user entry is set — otherwise it could run the
 * bootstrap entry and exit before that setup lands.
 *
 * *out_pid receives the probe's pid.  Returns 0 once the process is running, and
 * -1 for a NULL out_pid, an empty probe binary, or any failure along that setup
 * chain — a failure leaves the process parked with the bootstrap entry, which
 * exits with status -1 if it is ever unparked.  Nothing here waits for or checks
 * the probe's outcome. */
int kernel_ring3_spawn_native_probe(uint32_t parent_pid, uint32_t* out_pid) {
    process_t* proc = 0;
    mm_context_t* ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    const uint8_t* src = _binary_ring3_native_probe_bin_start;
    uint32_t code_size = (uint32_t)((uintptr_t)_binary_ring3_native_probe_bin_end -
                                    (uintptr_t)_binary_ring3_native_probe_bin_start);

    if (!out_pid || !src || code_size == 0) {
        return -1;
    }
    if (process_spawn_as_parked(parent_pid, "ring3-native", ring3_probe_bootstrap_entry, 0,
                                out_pid) != 0) {
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
    if (map_linear_pages(ctx->root_table, linear.base, linear.phys_base, code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) !=
        0) {
        return -1;
    }
    if (mm_copy_to_user(proc->context_id, linear.base, src, code_size) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table, linear.base, linear.phys_base, code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    list_iter_t it;
    mem_region_t* region = (mem_region_t*)list_first(&ctx->regions, &it);
    while (region) {
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
        region = (mem_region_t*)list_next(&it);
    }
    /* Map the top stack page user-writable explicitly: the mapping
     * mm_context_alloc_region installed is not guaranteed to survive the
     * low-slot strip process_set_user_entry performs
     * (paging_strip_low_slot_in_root), and the probe's first push would then
     * fault. Every spawn path in this file repeats this step. */
    uint64_t stack_top_page_virt = (stack.base + stack.size - 1u) & ~0xFFFULL;
    uint64_t stack_top_page_phys = (stack.phys_base + stack.size - 1u) & ~0xFFFULL;
    if (map_linear_pages(ctx->root_table, stack_top_page_virt, stack_top_page_phys, 0x1000u,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) !=
        0) {
        return -1;
    }

    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    /* Unpark only after full ring-3 setup is complete so the process cannot run
     * ring3_probe_bootstrap_entry and exit before that setup lands. */
    process_unpark_pid(*out_pid);
    klog_printf("[kernel] ring3 native pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}

/* As kernel_ring3_spawn_native_probe, but loads ring3_thread_lifecycle_probe.bin
 * — the probe that exercises the spawn/join/detach orderings from ring 3.  Same
 * parked-until-ready sequence, same 0-once-running / -1-on-any-setup-failure
 * convention, and the same absence of any outcome check. */
int kernel_ring3_spawn_thread_lifecycle_probe(uint32_t parent_pid, uint32_t* out_pid) {
    process_t* proc = 0;
    mm_context_t* ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    const uint8_t* src = _binary_ring3_thread_lifecycle_probe_bin_start;
    uint32_t code_size = (uint32_t)((uintptr_t)_binary_ring3_thread_lifecycle_probe_bin_end -
                                    (uintptr_t)_binary_ring3_thread_lifecycle_probe_bin_start);

    if (!out_pid || !src || code_size == 0) {
        return -1;
    }
    if (process_spawn_as_parked(parent_pid, "ring3-threading", ring3_probe_bootstrap_entry, 0,
                                out_pid) != 0) {
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
    if (map_linear_pages(ctx->root_table, linear.base, linear.phys_base, code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) !=
        0) {
        return -1;
    }
    if (mm_copy_to_user(proc->context_id, linear.base, src, code_size) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table, linear.base, linear.phys_base, code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    list_iter_t it;
    mem_region_t* region = (mem_region_t*)list_first(&ctx->regions, &it);
    while (region) {
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
        region = (mem_region_t*)list_next(&it);
    }

    uint64_t stack_top_page_virt = (stack.base + stack.size - 1u) & ~0xFFFULL;
    uint64_t stack_top_page_phys = (stack.phys_base + stack.size - 1u) & ~0xFFFULL;
    if (map_linear_pages(ctx->root_table, stack_top_page_virt, stack_top_page_phys, 0x1000u,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) !=
        0) {
        return -1;
    }

    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    process_unpark_pid(*out_pid);
    klog_printf("[kernel] ring3 threading pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}

static int spawn_ring3_fault_probe_named(uint32_t parent_pid, const char* name, const uint8_t* code,
                                         uint32_t code_size, uint32_t* out_pid) {
    process_t* proc = 0;
    mm_context_t* ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t stack_top_page_virt = 0;
    uint64_t stack_top_page_phys = 0;
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    if (!out_pid || !name || !code || code_size == 0) {
        return -1;
    }
    if (process_spawn_as_parked(parent_pid, name, ring3_probe_bootstrap_entry, 0, out_pid) != 0) {
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
    if (map_linear_pages(ctx->root_table, linear.base, linear.phys_base, code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) !=
        0) {
        return -1;
    }
    if (mm_copy_to_user(proc->context_id, linear.base, code, code_size) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table, linear.base, linear.phys_base, code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    stack_top_page_virt = (stack.base + stack.size - 1u) & ~0xFFFULL;
    stack_top_page_phys = (stack.phys_base + stack.size - 1u) & ~0xFFFULL;
    if (map_linear_pages(ctx->root_table, stack_top_page_virt, stack_top_page_phys, 0x1000u,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) !=
        0) {
        return -1;
    }
    list_iter_t it;
    mem_region_t* region = (mem_region_t*)list_first(&ctx->regions, &it);
    while (region) {
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
        region = (mem_region_t*)list_next(&it);
    }
    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    process_unpark_pid(*out_pid);
    klog_printf("[kernel] %s pid=%016llx\n", name, (unsigned long long)*out_pid);
    return 0;
}

/* The twelve fault probes below share one shape. Each blob opens with
 * `mov eax, WASMOS_SYSCALL_GETPID; int 0x80`, so a probe that never reaches
 * ring 3 is distinguishable from one that reached it and faulted as intended;
 * then comes the single instruction sequence that raises the vector named in
 * the function (#PF read/write/exec, #UD, #GP, #DE, #DB, #BP, #OF, #NM, #SS,
 * #AC); and each ends in `EB FE`, a self-jump that is reached only if the fault
 * fails to arrive -- the process then spins instead of running off into
 * whatever follows, and the policy runtime never sees an exit status.
 *
 * They share one signature too: the probe is spawned as a child of parent_pid,
 * *out_pid receives its pid, and the return is 0 once the process is unparked
 * and running, -1 for a NULL out_pid or any failure during spawn, region lookup,
 * code copy or mapping.  A 0 says the probe STARTED, never that it produced the
 * fault it is named for. */
int kernel_ring3_spawn_fault_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x48, 0x8B,
                                   0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault", code, (uint32_t)sizeof(code),
                                         out_pid);
}

int kernel_ring3_spawn_fault_write_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xC7, 0x05, 0x00,
                                   0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-write", code,
                                         (uint32_t)sizeof(code), out_pid);
}

int kernel_ring3_spawn_fault_exec_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x50, 0x48,
                                   0x8D, 0x44, 0x24, 0x00, 0xFF, 0xE0, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-exec", code,
                                         (uint32_t)sizeof(code), out_pid);
}

int kernel_ring3_spawn_fault_ud_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD,
                                   0x80, 0x0F, 0x0B, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-ud", code, (uint32_t)sizeof(code),
                                         out_pid);
}

int kernel_ring3_spawn_fault_gp_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xFA, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-gp", code, (uint32_t)sizeof(code),
                                         out_pid);
}

int kernel_ring3_spawn_fault_de_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x31,
                                   0xD2, 0x31, 0xC0, 0xF7, 0xF2, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-de", code, (uint32_t)sizeof(code),
                                         out_pid);
}

int kernel_ring3_spawn_fault_db_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xF1, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-db", code, (uint32_t)sizeof(code),
                                         out_pid);
}

int kernel_ring3_spawn_fault_bp_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xCC, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-bp", code, (uint32_t)sizeof(code),
                                         out_pid);
}

int kernel_ring3_spawn_fault_of_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80,
                                   0xCD, 0x04, 0x0F, 0x0B, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-of", code, (uint32_t)sizeof(code),
                                         out_pid);
}

int kernel_ring3_spawn_fault_nm_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80,
                                   0xD9, 0xE8, 0x0F, 0x0B, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-nm", code, (uint32_t)sizeof(code),
                                         out_pid);
}

int kernel_ring3_spawn_fault_ss_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x48,
                                   0xBC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                                   0x00, 0x50, 0x0F, 0x0B, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-ss", code, (uint32_t)sizeof(code),
                                         out_pid);
}

int kernel_ring3_spawn_fault_ac_probe(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x9C, 0x58, 0x48,
                                   0x0D, 0x00, 0x00, 0x04, 0x00, 0x50, 0x9D, 0x48, 0x8D, 0x44,
                                   0x24, 0x01, 0x8B, 0x00, 0x0F, 0x0B, 0xEB, 0xFE};
    return spawn_ring3_fault_probe_named(parent_pid, "ring3-fault-ac", code, (uint32_t)sizeof(code),
                                         out_pid);
}
