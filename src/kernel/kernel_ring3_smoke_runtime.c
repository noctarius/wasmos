#include "kernel_ring3_smoke_runtime.h"

#include "ipc.h"
#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "physmem.h"
#include "process.h"
#include "process_manager.h"
#include "syscall.h"

#include <string.h>

static process_run_result_t
ring3_probe_bootstrap_entry(process_t *process, void *arg)
{
    (void)arg;
    if (process) {
        process_set_exit_status(process, -1);
    }
    return PROCESS_RUN_EXITED;
}

void
kernel_shmem_owner_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id)
{
    uint32_t shmem_id = 0;
    uint64_t phys = 0;
    uint64_t pages = 0;

    if (owner_context_id == 0 || foreign_context_id == 0 || owner_context_id == foreign_context_id) {
        return;
    }
    if (mm_shared_create(owner_context_id, 1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                         &shmem_id, &phys) != 0 || shmem_id == 0 || phys == 0) {
        klog_write("[test] shmem owner setup failed\n");
        return;
    }
    if (mm_shared_retain(owner_context_id, shmem_id) != 0) {
        klog_write("[test] shmem owner setup failed\n");
        return;
    }
    if (mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) == 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id) == 0) {
        klog_write("[test] shmem owner deny mismatch\n");
    } else {
        klog_write("[test] shmem owner deny ok\n");
    }
    if (mm_shared_grant(owner_context_id, shmem_id, foreign_context_id) != 0 ||
        mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) != 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) != 0 ||
        mm_shared_release(foreign_context_id, shmem_id) != 0) {
        klog_write("[test] shmem grant allow mismatch\n");
    } else {
        klog_write("[test] shmem grant allow ok\n");
    }
    if (mm_shared_release(owner_context_id, shmem_id) != 0) {
        klog_write("[test] shmem owner cleanup failed\n");
    }
}

void
kernel_ring3_shmem_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id)
{
    uint32_t shmem_id = 0;
    uint64_t phys = 0;
    uint64_t pages = 0;

    if (owner_context_id == 0 || foreign_context_id == 0 || owner_context_id == foreign_context_id) {
        klog_write("[test] ring3 shmem setup failed\n");
        return;
    }
    if (mm_shared_create(owner_context_id, 1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                         &shmem_id, &phys) != 0 || shmem_id == 0 || phys == 0) {
        klog_write("[test] ring3 shmem setup failed\n");
        return;
    }
    if (mm_shared_retain(owner_context_id, shmem_id) != 0) {
        klog_write("[test] ring3 shmem setup failed\n");
        return;
    }
    if (mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) == 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id) == 0) {
        klog_write("[test] ring3 shmem owner deny mismatch\n");
    } else {
        klog_write("[test] ring3 shmem owner deny ok\n");
    }
    if (mm_shared_grant(owner_context_id, shmem_id, foreign_context_id) != 0 ||
        mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) != 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) != 0 ||
        mm_shared_release(foreign_context_id, shmem_id) != 0) {
        klog_write("[test] ring3 shmem grant allow mismatch\n");
    } else {
        klog_write("[test] ring3 shmem grant allow ok\n");
    }
    if (mm_shared_release(owner_context_id, shmem_id) != 0) {
        klog_write("[test] ring3 shmem cleanup failed\n");
    }
}

void
kernel_shmem_misuse_matrix_test(uint32_t owner_context_id, uint32_t foreign_context_id, uint8_t ring3_mode)
{
    uint32_t shmem_id = 0;
    uint64_t phys = 0;
    uint64_t pages = 0;
    uint64_t map_base = 0;
    uint8_t ok = 1;
    mm_context_t *foreign_ctx = 0;

    if (owner_context_id == 0 || foreign_context_id == 0 || owner_context_id == foreign_context_id) {
        return;
    }
    foreign_ctx = mm_context_get(foreign_context_id);
    if (!foreign_ctx) {
        ok = 0;
        goto done;
    }
    if (mm_shared_create(owner_context_id, 1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                         &shmem_id, &phys) != 0 || shmem_id == 0 || phys == 0) {
        ok = 0;
        goto done;
    }
    if (mm_shared_retain(owner_context_id, shmem_id) != 0) {
        ok = 0;
        goto done;
    }
    if (mm_shared_get_phys(foreign_context_id, shmem_id + 0x100, &phys, &pages) == 0 ||
        mm_shared_retain(foreign_context_id, shmem_id + 0x100) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id + 0x100) == 0 ||
        mm_shared_map(foreign_ctx, shmem_id + 0x100, MEM_REGION_FLAG_READ, &map_base) == 0) {
        ok = 0;
    }
    if (mm_shared_grant(foreign_context_id, shmem_id, owner_context_id) == 0 ||
        mm_shared_revoke(foreign_context_id, shmem_id, owner_context_id) == 0) {
        ok = 0;
    }
    if (mm_shared_map(foreign_ctx, shmem_id, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, &map_base) == 0) {
        ok = 0;
    }
    if (mm_shared_grant(owner_context_id, shmem_id, foreign_context_id) != 0) {
        ok = 0;
    }
    if (mm_shared_map(foreign_ctx, shmem_id, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, &map_base) != 0 ||
        map_base == 0 ||
        mm_shared_unmap(foreign_ctx, shmem_id) != 0) {
        ok = 0;
    }
    if (mm_shared_revoke(owner_context_id, shmem_id, foreign_context_id) != 0 ||
        mm_shared_revoke(owner_context_id, shmem_id, foreign_context_id) != 0) {
        ok = 0;
    }
    if (mm_shared_map(foreign_ctx, shmem_id, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, &map_base) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id) == 0) {
        ok = 0;
    }

done:
    if (shmem_id) {
        if (mm_shared_release(owner_context_id, shmem_id) != 0) {
            ok = 0;
        }
        if (mm_shared_release(owner_context_id, shmem_id) == 0) {
            ok = 0;
        }
    }
    if (ring3_mode) {
        klog_write(ok ? "[test] ring3 shmem misuse matrix ok\n"
                        : "[test] ring3 shmem misuse matrix mismatch\n");
    } else {
        klog_write(ok ? "[test] shmem misuse matrix ok\n"
                        : "[test] shmem misuse matrix mismatch\n");
    }
}

static int
map_linear_pages(uint64_t root_table, uint64_t virt_base, uint64_t phys_base, uint32_t size, uint32_t map_flags)
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

int
kernel_ring3_spawn_smoke_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_code[] = {
        0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0x48, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xBF, 0x00, 0x00, 0x00, 0x00, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xBF, 0x00, 0x00, 0x00, 0x00, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBE, 0x21, 0x43, 0x00, 0x00, 0xBA, 0xBE, 0xBA, 0xFE, 0xCA, 0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x55, 0x22, 0x00, 0x00, 0xBA, 0xCD, 0xAB, 0x00, 0x00, 0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x56, 0x66, 0x00, 0x00, 0xBA, 0xC0, 0xDE, 0x00, 0x00, 0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x78, 0x56, 0x00, 0x00, 0xBA, 0xEF, 0xBE, 0xAD, 0xDE, 0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0xBC, 0x9A, 0x00, 0x00, 0xBA, 0x78, 0x56, 0x34, 0x12, 0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0xBD, 0x9A, 0x00, 0x00, 0xBA, 0x0D, 0xF0, 0xAD, 0x0B, 0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xB8, 0x03, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xB9, 0x00, 0x40, 0x00, 0x00, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xFF, 0xC9, 0x75, 0xF5, 0x31, 0xFF, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xEB, 0xFE
    };
    uint8_t ring3_code_patched[sizeof(ring3_code)];
    process_t *proc = 0;
    mm_context_t *ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    uint32_t ring3_notify_ep = IPC_ENDPOINT_NONE;
    uint32_t ring3_notify_control_ep = IPC_ENDPOINT_NONE;
    uint32_t ring3_call_denied_ep = IPC_ENDPOINT_NONE;
    uint32_t ring3_call_control_ep = IPC_ENDPOINT_NONE;
    uint32_t ring3_call_echo_ep = IPC_ENDPOINT_NONE;

    if (!out_pid) {
        return -1;
    }
    if (process_spawn_as(parent_pid, "ring3-smoke", ring3_probe_bootstrap_entry, 0, out_pid) != 0) {
        return -1;
    }
    proc = process_get(*out_pid);
    if (!proc) {
        return -1;
    }
    if (ipc_notification_create(proc->context_id, &ring3_notify_ep) != IPC_OK ||
        ipc_notification_create(IPC_CONTEXT_KERNEL, &ring3_notify_control_ep) != IPC_OK ||
        ipc_endpoint_create(IPC_CONTEXT_KERNEL, &ring3_call_denied_ep) != IPC_OK ||
        ipc_endpoint_create(IPC_CONTEXT_KERNEL, &ring3_call_echo_ep) != IPC_OK) {
        return -1;
    }
    ring3_call_control_ep = process_manager_endpoint();
    if (ring3_notify_ep == IPC_ENDPOINT_NONE || ring3_notify_control_ep == IPC_ENDPOINT_NONE ||
        ring3_call_denied_ep == IPC_ENDPOINT_NONE || ring3_call_echo_ep == IPC_ENDPOINT_NONE ||
        ring3_call_control_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    syscall_set_ipc_call_echo_endpoint(ring3_call_echo_ep);
    syscall_set_ipc_call_control_deny_endpoint(ring3_call_control_ep);
    syscall_set_ipc_notify_control_deny_endpoint(ring3_notify_control_ep);
    ctx = mm_context_get(proc->context_id);
    if (!ctx ||
        mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0 ||
        linear.phys_base == 0 || linear.size < sizeof(ring3_code) || stack.base == 0 || stack.size < 16u) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table, linear.base, linear.phys_base,
                         (uint32_t)sizeof(ring3_code),
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    memcpy(ring3_code_patched, ring3_code, sizeof(ring3_code_patched));
    {
        const uint32_t offsets[] = {30u, 42u, 76u, 98u, 120u, 142u, 164u};
        const uint32_t values[] = {
            ring3_notify_ep, ring3_notify_control_ep, ring3_call_denied_ep,
            ring3_call_control_ep, ring3_call_echo_ep, ring3_call_echo_ep, ring3_call_echo_ep
        };
        for (uint32_t i = 0; i < 7u; ++i) {
            uint32_t value = values[i];
            uint32_t offset = offsets[i];
            ring3_code_patched[offset + 0] = (uint8_t)(value & 0xFFu);
            ring3_code_patched[offset + 1] = (uint8_t)((value >> 8) & 0xFFu);
            ring3_code_patched[offset + 2] = (uint8_t)((value >> 16) & 0xFFu);
            ring3_code_patched[offset + 3] = (uint8_t)((value >> 24) & 0xFFu);
        }
    }
    if (mm_copy_to_user(proc->context_id, linear.base, ring3_code_patched, (uint32_t)sizeof(ring3_code_patched)) != 0 ||
        map_linear_pages(ctx->root_table, linear.base, linear.phys_base,
                         (uint32_t)sizeof(ring3_code),
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
    klog_printf("[kernel] ring3 smoke pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}
