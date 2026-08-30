/* kernel_ring3_smoke_runtime.c - Ring-3 IPC smoke process and its permission
 * tests.
 *
 * kernel_ring3_spawn_smoke_process loads a hand-assembled ring-3 blob (no WASM,
 * no ESP, no process manager) that drives the ipc_notify and ipc_call syscalls
 * against endpoints the kernel deliberately hands it: its own, kernel-owned
 * ones it must be refused, and an echo endpoint it may use. The kernel side of
 * the check is in syscall.c, which recognises the process by its "ring3-smoke"
 * name and logs a "[test] ring3 ipc ..." marker for each expected outcome,
 * either "... ok" or "... mismatch", for the boot-time harness to grep. */
#include "kernel_ring3_smoke_runtime.h"

#include "ipc.h"
#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "process_manager.h"
#include "syscall.h"

#include <string.h>

static process_run_result_t ring3_probe_bootstrap_entry(process_t* process, void* arg) {
    (void)arg;
    if (process) {
        process_set_exit_status(process, -1);
    }
    return PROCESS_RUN_EXITED;
}

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

/* The blob runs, in order: four ipc_notify calls (an endpoint id of all-ones, a
 * 64-bit id that does not fit u32, this process's own notification endpoint, a
 * kernel-owned one), six ipc_call calls (invalid destination, kernel-owned
 * destination, the process-manager endpoint, then three to the echo endpoint),
 * a yield, 16384 getpid calls to be preempted inside the gate, and exit(0).
 * The two final echo calls carry message types 0x9ABC and 0x9ABD, which
 * syscall.c keys on to inject stale/synthetic and out-of-order/forged replies:
 * those constants are protocol between the blob and syscall.c, not filler. */
int kernel_ring3_spawn_smoke_process(uint32_t parent_pid, uint32_t* out_pid) {
    static const uint8_t ring3_code[] = {
        0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x48, 0xBF, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xBF,
        0x00, 0x00, 0x00, 0x00, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xBF, 0x00, 0x00, 0x00,
        0x00, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBE, 0x21,
        0x43, 0x00, 0x00, 0xBA, 0xBE, 0xBA, 0xFE, 0xCA, 0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x55, 0x22, 0x00, 0x00, 0xBA, 0xCD, 0xAB, 0x00, 0x00,
        0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x56, 0x66,
        0x00, 0x00, 0xBA, 0xC0, 0xDE, 0x00, 0x00, 0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xBF,
        0x00, 0x00, 0x00, 0x00, 0xBE, 0x78, 0x56, 0x00, 0x00, 0xBA, 0xEF, 0xBE, 0xAD, 0xDE, 0xB8,
        0x06, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0xBC, 0x9A, 0x00,
        0x00, 0xBA, 0x78, 0x56, 0x34, 0x12, 0xB8, 0x06, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xBF, 0x00,
        0x00, 0x00, 0x00, 0xBE, 0xBD, 0x9A, 0x00, 0x00, 0xBA, 0x0D, 0xF0, 0xAD, 0x0B, 0xB8, 0x06,
        0x00, 0x00, 0x00, 0xCD, 0x80, 0xB8, 0x03, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xB9, 0x00, 0x40,
        0x00, 0x00, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xFF, 0xC9, 0x75, 0xF5, 0x31, 0xFF,
        0xB8, 0x02, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xEB, 0xFE};
    uint8_t ring3_code_patched[sizeof(ring3_code)];
    process_t* proc = 0;
    mm_context_t* ctx = 0;
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
    if (!ctx || mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0 || linear.phys_base == 0 ||
        linear.size < sizeof(ring3_code) || stack.base == 0 || stack.size < 16u) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         (uint32_t)sizeof(ring3_code),
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) !=
        0) {
        return -1;
    }
    memcpy(ring3_code_patched, ring3_code, sizeof(ring3_code_patched));

    /* Endpoint ids are only known at spawn time, so they are patched into the
     * blob's `mov edi, imm32` operands. Each offset is the byte position of one
     * such immediate and pairs positionally with values[]; the two arrays and
     * the loop bound must be updated together whenever the blob changes. */
    const uint32_t offsets[] = {30u, 42u, 76u, 98u, 120u, 142u, 164u};
    const uint32_t values[] = {ring3_notify_ep,
                               ring3_notify_control_ep,
                               ring3_call_denied_ep,
                               ring3_call_control_ep,
                               ring3_call_echo_ep,
                               ring3_call_echo_ep,
                               ring3_call_echo_ep};
    for (uint32_t i = 0; i < 7u; ++i) {
        uint32_t value = values[i];
        uint32_t offset = offsets[i];
        ring3_code_patched[offset + 0] = (uint8_t)(value & 0xFFu);
        ring3_code_patched[offset + 1] = (uint8_t)((value >> 8) & 0xFFu);
        ring3_code_patched[offset + 2] = (uint8_t)((value >> 16) & 0xFFu);
        ring3_code_patched[offset + 3] = (uint8_t)((value >> 24) & 0xFFu);
    }

    if (mm_copy_to_user(proc->context_id,
                        linear.base,
                        ring3_code_patched,
                        (uint32_t)sizeof(ring3_code_patched)) != 0 ||
        map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         (uint32_t)sizeof(ring3_code),
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
    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    klog_printf("[kernel] ring3 smoke pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}
