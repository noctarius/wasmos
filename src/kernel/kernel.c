#include "boot.h"
#include "cpu.h"
#include "ipc.h"
#include "memory.h"
#include "memory_service.h"
#include "paging.h"
#include "process.h"
#include "thread.h"
#include "process_manager.h"
#include "syscall.h"
#include "serial.h"
#include "timer.h"
#include "wasmos_app.h"
#include "wasm_chardev.h"
#include "wasm3_probe.h"
#include "wasm3_link.h"
#include "physmem.h"
#include "io.h"
#include "framebuffer.h"
#include "capability.h"
#include "slab.h"
#include "kernel_init_runtime.h"
#include "kernel_boot_runtime.h"
#include "kernel_selftest_runtime.h"
#include "kernel_threading_selftest_runtime.h"
#include "kernel_ring3_fault_runtime.h"
#include "kernel_ring3_probe_runtime.h"

#include <stdint.h>
#include <string.h>
#include "wasm3.h"

/*
 * kernel.c owns the high-level bootstrap choreography after the architecture
 * entry path has established a stack and cleared BSS. The file intentionally
 * keeps policy limited to early bring-up and the kernel-owned init task.
 */

static uint32_t g_chardev_service_endpoint = IPC_ENDPOINT_NONE;
static const boot_info_t *g_boot_info;
static boot_info_t g_boot_info_shadow;

static const uint8_t g_preempt_test_enabled = 0;
#ifndef WASMOS_RING3_SMOKE_DEFAULT
#define WASMOS_RING3_SMOKE_DEFAULT 0
#endif
#ifndef WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFAULT
#define WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFAULT 0
#endif
/* Keep adversarial smoke probes opt-in for dedicated ring3 test targets; they
 * add noise to normal boot/CLI workflows and are not required for baseline
 * ring3 policy mode. */
static const uint8_t g_ring3_smoke_enabled = WASMOS_RING3_SMOKE_DEFAULT;
static const uint8_t g_ring3_thread_lifecycle_smoke_enabled =
    WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFAULT;

uint8_t
kernel_ring3_smoke_enabled(void)
{
    return g_ring3_smoke_enabled;
}

static const uint8_t g_ring3_fault_churn_rounds = 6;

static init_state_t g_init_state;

static int
boot_info_build_shadow(const boot_info_t *src, boot_info_t *dst);
static int
spawn_ring3_fault_churn_probe_process(uint32_t parent_pid, uint8_t churn_round, uint32_t *out_pid);
static void
run_shmem_owner_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id);
static void
run_shmem_misuse_matrix_test(uint32_t owner_context_id, uint32_t foreign_context_id, uint8_t ring3_mode);

static int
boot_info_build_shadow(const boot_info_t *src, boot_info_t *dst)
{
    return kernel_boot_build_bootinfo_shadow(src, dst);
}

static void
run_low_slot_sweep_diagnostic(void)
{
    kernel_boot_run_low_slot_sweep_diagnostic();
}

static int
wasmos_endpoint_resolve(uint32_t owner_context_id,
                        const uint8_t *name,
                        uint32_t name_len,
                        uint32_t rights,
                        uint32_t *out_endpoint)
{
    (void)owner_context_id;
    (void)rights;
    if (!out_endpoint) {
        return -1;
    }
    if (str_eq_bytes(name, name_len, "chardev") &&
        g_chardev_service_endpoint != IPC_ENDPOINT_NONE) {
        *out_endpoint = g_chardev_service_endpoint;
        return 0;
    }
    if (str_eq_bytes(name, name_len, "proc")) {
        uint32_t proc_ep = process_manager_endpoint();
        if (proc_ep != IPC_ENDPOINT_NONE) {
            *out_endpoint = proc_ep;
            return 0;
        }
    }
    if (str_eq_bytes(name, name_len, "block")) {
        uint32_t block_ep = process_manager_block_endpoint();
        if (block_ep != IPC_ENDPOINT_NONE) {
            *out_endpoint = block_ep;
            return 0;
        }
    }
    if (str_eq_bytes(name, name_len, "fs")) {
        uint32_t fs_ep = process_manager_fs_endpoint();
        if (fs_ep != IPC_ENDPOINT_NONE) {
            *out_endpoint = fs_ep;
            return 0;
        }
    }
    return -1;
}

static int
wasmos_capability_grant(uint32_t owner_context_id,
                        const uint8_t *name,
                        uint32_t name_len,
                        uint32_t flags)
{
    if (str_eq_bytes(name, name_len, "ipc.basic")) {
        return 0;
    }
    if (capability_grant_name(owner_context_id, name, name_len, flags) == 0) {
        return 0;
    }
    return -1;
}

static process_run_result_t
chardev_server_entry(process_t *process, void *arg)
{
    (void)process;
    (void)arg;

    int rc = wasm_chardev_run();
    process_set_exit_status(process, rc == 0 ? 0 : -1);
    return PROCESS_RUN_EXITED;
}

static process_run_result_t
idle_entry(process_t *process, void *arg)
{
    (void)process;
    (void)arg;
    for (;;) {
        __asm__ volatile("hlt");
    }
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

static void
run_shmem_owner_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id)
{
    uint32_t shmem_id = 0;
    uint64_t phys = 0;
    uint64_t pages = 0;

    if (owner_context_id == 0 || foreign_context_id == 0 || owner_context_id == foreign_context_id) {
        return;
    }
    if (mm_shared_create(owner_context_id, 1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                         &shmem_id, &phys) != 0 || shmem_id == 0 || phys == 0) {
        serial_write("[test] shmem owner setup failed\n");
        return;
    }
    if (mm_shared_retain(owner_context_id, shmem_id) != 0) {
        serial_write("[test] shmem owner setup failed\n");
        return;
    }
    if (mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) == 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id) == 0) {
        serial_write("[test] shmem owner deny mismatch\n");
    } else {
        serial_write("[test] shmem owner deny ok\n");
    }
    if (mm_shared_grant(owner_context_id, shmem_id, foreign_context_id) != 0 ||
        mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) != 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) != 0 ||
        mm_shared_release(foreign_context_id, shmem_id) != 0) {
        serial_write("[test] shmem grant allow mismatch\n");
    } else {
        serial_write("[test] shmem grant allow ok\n");
    }
    if (mm_shared_release(owner_context_id, shmem_id) != 0) {
        serial_write("[test] shmem owner cleanup failed\n");
    }
}

static void
run_ring3_shmem_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id)
{
    uint32_t shmem_id = 0;
    uint64_t phys = 0;
    uint64_t pages = 0;

    if (owner_context_id == 0 || foreign_context_id == 0 || owner_context_id == foreign_context_id) {
        serial_write("[test] ring3 shmem setup failed\n");
        return;
    }
    if (mm_shared_create(owner_context_id, 1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                         &shmem_id, &phys) != 0 || shmem_id == 0 || phys == 0) {
        serial_write("[test] ring3 shmem setup failed\n");
        return;
    }
    if (mm_shared_retain(owner_context_id, shmem_id) != 0) {
        serial_write("[test] ring3 shmem setup failed\n");
        return;
    }
    if (mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) == 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id) == 0) {
        serial_write("[test] ring3 shmem owner deny mismatch\n");
    } else {
        serial_write("[test] ring3 shmem owner deny ok\n");
    }
    if (mm_shared_grant(owner_context_id, shmem_id, foreign_context_id) != 0 ||
        mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) != 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) != 0 ||
        mm_shared_release(foreign_context_id, shmem_id) != 0) {
        serial_write("[test] ring3 shmem grant allow mismatch\n");
    } else {
        serial_write("[test] ring3 shmem grant allow ok\n");
    }
    if (mm_shared_release(owner_context_id, shmem_id) != 0) {
        serial_write("[test] ring3 shmem cleanup failed\n");
    }
}

static void
run_shmem_misuse_matrix_test(uint32_t owner_context_id, uint32_t foreign_context_id, uint8_t ring3_mode)
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
        serial_write(ok ? "[test] ring3 shmem misuse matrix ok\n"
                        : "[test] ring3 shmem misuse matrix mismatch\n");
    } else {
        serial_write(ok ? "[test] shmem misuse matrix ok\n"
                        : "[test] shmem misuse matrix mismatch\n");
    }
}

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

static int
spawn_ring3_smoke_process(uint32_t parent_pid, uint32_t *out_pid)
{
    /* Ring3 stress loop:
     * - probe IPC syscall boundary with an invalid notify endpoint (deny path)
     *   and a process-owned notification endpoint (allow path)
     * - probe IPC call boundary with invalid/permission-denied endpoints and a
     *   kernel echo endpoint (allow path)
     * - issue an explicit YIELD syscall from CPL3
     * - execute many GETPID syscalls from CPL3 to exercise timer-IRQ preempt
     *   + trampoline return under repeated user->kernel transitions
     * - exit cleanly once done. */
    static const uint8_t ring3_code[] = {
        0xBF, 0xFF, 0xFF, 0xFF, 0xFF, /* mov edi, 0xFFFFFFFF (invalid ep) */
        0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_NOTIFY */
        0xCD, 0x80,                   /* int 0x80 */
        0x48, 0xBF,                   /* mov rdi, 0x0000000100000000 (arg width invalid) */
        0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_NOTIFY */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <ring3 notify ep> (patched) */
        0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_NOTIFY */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel notify control ep> (patched) */
        0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_NOTIFY */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0xFF, 0xFF, 0xFF, 0xFF, /* mov edi, 0xFFFFFFFF (invalid ep) */
        0xBE, 0x21, 0x43, 0x00, 0x00, /* mov esi, 0x4321 (msg type) */
        0xBA, 0xBE, 0xBA, 0xFE, 0xCA, /* mov edx, 0xCAFEBABE (arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel denied call ep> (patched) */
        0xBE, 0x55, 0x22, 0x00, 0x00, /* mov esi, 0x2255 (msg type) */
        0xBA, 0xCD, 0xAB, 0x00, 0x00, /* mov edx, 0x0000ABCD (arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <process-manager call ep> (patched) */
        0xBE, 0x56, 0x66, 0x00, 0x00, /* mov esi, 0x6656 (control deny probe type) */
        0xBA, 0xC0, 0xDE, 0x00, 0x00, /* mov edx, 0x0000DEC0 (arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel echo call ep> (patched) */
        0xBE, 0x78, 0x56, 0x00, 0x00, /* mov esi, 0x5678 (msg type) */
        0xBA, 0xEF, 0xBE, 0xAD, 0xDE, /* mov edx, 0xDEADBEEF (arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel echo call ep> (patched) */
        0xBE, 0xBC, 0x9A, 0x00, 0x00, /* mov esi, 0x9ABC (correlation probe type) */
        0xBA, 0x78, 0x56, 0x34, 0x12, /* mov edx, 0x12345678 (probe arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel echo call ep> (patched) */
        0xBE, 0xBD, 0x9A, 0x00, 0x00, /* mov esi, 0x9ABD (source-auth probe type) */
        0xBA, 0x0D, 0xF0, 0xAD, 0x0B, /* mov edx, 0x0BADF00D (probe arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xB8, 0x03, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_YIELD */
        0xCD, 0x80,                   /* int 0x80 */
        0xB9, 0x00, 0x40, 0x00, 0x00, /* mov ecx, 16384 */
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0xFF, 0xC9,                   /* dec ecx */
        0x75, 0xF5,                   /* jnz <mov eax, GETPID> */
        0x31, 0xFF,                   /* xor edi, edi (exit status 0) */
        0xB8, 0x02, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_EXIT */
        0xCD, 0x80,                   /* int 0x80 */
        0xEB, 0xFE                    /* should not return: spin if it does */
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
        ring3_notify_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (ipc_notification_create(IPC_CONTEXT_KERNEL, &ring3_notify_control_ep) != IPC_OK ||
        ring3_notify_control_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &ring3_call_denied_ep) != IPC_OK ||
        ring3_call_denied_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &ring3_call_echo_ep) != IPC_OK ||
        ring3_call_echo_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    ring3_call_control_ep = process_manager_endpoint();
    if (ring3_call_control_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    syscall_set_ipc_call_echo_endpoint(ring3_call_echo_ep);
    syscall_set_ipc_call_control_deny_endpoint(ring3_call_control_ep);
    syscall_set_ipc_notify_control_deny_endpoint(ring3_notify_control_ep);
    ctx = mm_context_get(proc->context_id);
    if (!ctx) {
        return -1;
    }
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0) {
        return -1;
    }
    if (linear.phys_base == 0 || linear.size < sizeof(ring3_code) || stack.base == 0 || stack.size < 16u) {
        return -1;
    }

    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         (uint32_t)sizeof(ring3_code),
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    memcpy(ring3_code_patched, ring3_code, sizeof(ring3_code_patched));
    /* Patch mov edi immediate for the valid ring3-owned notification endpoint.
     * Layout offset: first mov(5) + mov eax(5) + int80(2) + mov edi opcode(1). */
    {
        const uint32_t ep_imm_off = 30u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_notify_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_notify_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_notify_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_notify_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 42u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_notify_control_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_notify_control_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_notify_control_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_notify_control_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 76u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_denied_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_denied_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_denied_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_denied_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 98u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_control_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_control_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_control_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_control_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 120u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_echo_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_echo_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_echo_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_echo_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 142u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_echo_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_echo_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_echo_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_echo_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 164u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_echo_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_echo_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_echo_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_echo_ep >> 24) & 0xFFu);
    }
    if (mm_copy_to_user(proc->context_id,
                        linear.base,
                        ring3_code_patched,
                        (uint32_t)sizeof(ring3_code_patched)) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
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

    serial_printf("[kernel] ring3 smoke pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}

static int
spawn_ring3_fault_churn_probe_process(uint32_t parent_pid, uint8_t churn_round, uint32_t *out_pid)
{
    if ((churn_round & 1u) == 0u) {
        return kernel_ring3_spawn_fault_ud_probe(parent_pid, out_pid);
    }
    return kernel_ring3_spawn_fault_gp_probe(parent_pid, out_pid);
}

static void
run_kernel_loop(void)
{
    kernel_boot_run_scheduler_loop();
}

void
kmain(boot_info_t *boot_info)
{
    uint32_t chardev_pid = 0;
    process_t *chardev_proc;
    uint32_t chardev_endpoint = IPC_ENDPOINT_NONE;
    uint32_t mem_service_pid = 0;
    process_t *mem_service_proc = 0;
    uint32_t mem_service_endpoint = IPC_ENDPOINT_NONE;
    uint32_t mem_reply_endpoint = IPC_ENDPOINT_NONE;
    uint32_t ring3_smoke_pid = 0;
    uint32_t ring3_native_pid = 0;
    uint32_t ring3_threading_pid = 0;
    uint32_t ring3_fault_pid = 0;
    uint32_t ring3_fault_write_pid = 0;
    uint32_t ring3_fault_exec_pid = 0;
    uint32_t ring3_fault_ud_pid = 0;
    uint32_t ring3_fault_gp_pid = 0;
    uint32_t ring3_fault_de_pid = 0;
    uint32_t ring3_fault_db_pid = 0;
    uint32_t ring3_fault_bp_pid = 0;
    uint32_t ring3_fault_of_pid = 0;
    uint32_t ring3_fault_nm_pid = 0;
    uint32_t ring3_fault_ss_pid = 0;
    uint32_t ring3_fault_ac_pid = 0;
    uint32_t idle_pid = 0;
    uint32_t init_pid = 0;

    (void)boot_info;

    serial_init();
    serial_write("[kernel] kmain\n");
    if (!boot_info || boot_info->version != BOOT_INFO_VERSION ||
        boot_info->size < sizeof(boot_info_t)) {
        serial_write("[kernel] invalid boot_info\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    serial_printf("[kernel] boot_info version=%016llx\n[kernel] boot_info size=%016llx\n",
        (unsigned long long)boot_info->version,
        (unsigned long long)boot_info->size);
    g_boot_info = boot_info;
    framebuffer_init(boot_info);
    cpu_init();

    mm_init(boot_info);
    serial_enable_high_alias(1);
    cpu_relocate_tables_high();
    capability_init();
    slab_init();
    if (boot_info_build_shadow(boot_info, &g_boot_info_shadow) != 0) {
        serial_write("[kernel] boot_info shadow copy failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    boot_info = &g_boot_info_shadow;
    g_boot_info = boot_info;
    ipc_init();
    process_init();
    wasm3_link_init(boot_info);

    serial_write("[kernel] wasm3 init on-demand\n");
    serial_write("[kernel] boot_info shadow active\n");

    if (process_spawn_idle("idle", idle_entry, 0, &idle_pid) != 0) {
        serial_write("[kernel] idle spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    kernel_init_state_reset(&g_init_state, boot_info);
    if (process_spawn("init", kernel_init_entry, &g_init_state, &init_pid) != 0) {
        serial_write("[kernel] init spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    serial_printf("[kernel] init pid=%016llx\n", (unsigned long long)init_pid);

    if (process_spawn_as(init_pid, "mem-service", memory_service_entry, 0, &mem_service_pid) != 0) {
        serial_write("[kernel] mem service spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    mem_service_proc = process_get(mem_service_pid);
    if (!mem_service_proc) {
        serial_write("[kernel] mem service lookup failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (ipc_endpoint_create(mem_service_proc->context_id, &mem_service_endpoint) != IPC_OK ||
        ipc_endpoint_create(IPC_CONTEXT_KERNEL, &mem_reply_endpoint) != IPC_OK) {
        serial_write("[kernel] mem service endpoint create failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    memory_service_register(mem_service_proc->context_id, mem_service_endpoint, mem_reply_endpoint);
    serial_write("[kernel] mem service ready\n");

    if (process_spawn_as(init_pid, "chardev-server", chardev_server_entry, 0, &chardev_pid) != 0) {
        serial_write("[kernel] chardev process spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    serial_printf("[kernel] chardev pid=%016llx\n", (unsigned long long)chardev_pid);

    chardev_proc = process_get(chardev_pid);
    if (!chardev_proc || wasm_chardev_init(chardev_proc->context_id) != 0) {
        serial_write("[kernel] chardev service init failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (wasm_chardev_endpoint(&chardev_endpoint) != 0) {
        serial_write("[kernel] chardev endpoint lookup failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    g_chardev_service_endpoint = chardev_endpoint;
    run_shmem_owner_isolation_test(mem_service_proc->context_id, chardev_proc->context_id);
    run_shmem_misuse_matrix_test(mem_service_proc->context_id,
                                 chardev_proc->context_id,
                                 g_ring3_smoke_enabled);

    wasmos_app_set_policy_hooks(wasmos_endpoint_resolve, wasmos_capability_grant);

    if (kernel_selftest_spawn_baseline(init_pid, g_preempt_test_enabled) != 0) {
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (kernel_threading_selftest_spawn(init_pid, g_ring3_thread_lifecycle_smoke_enabled) != 0) {
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (g_ring3_smoke_enabled) {
        if (spawn_ring3_smoke_process(init_pid, &ring3_smoke_pid) != 0) {
            serial_write("[kernel] ring3 smoke spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_native_probe(init_pid, &ring3_native_pid) != 0) {
            serial_write("[kernel] ring3 native spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (g_ring3_thread_lifecycle_smoke_enabled) {
            if (kernel_ring3_spawn_thread_lifecycle_probe(init_pid, &ring3_threading_pid) != 0) {
                serial_write("[kernel] ring3 threading spawn failed\n");
                for (;;) {
                    __asm__ volatile("hlt");
                }
            }
        }
        process_t *ring3_smoke_proc = process_get(ring3_smoke_pid);
        process_t *ring3_native_proc = process_get(ring3_native_pid);
        if (!ring3_smoke_proc || !ring3_native_proc) {
            serial_write("[test] ring3 shmem setup failed\n");
        } else {
            run_ring3_shmem_isolation_test(ring3_smoke_proc->context_id, ring3_native_proc->context_id);
        }
        if (kernel_ring3_spawn_fault_probe(init_pid, &ring3_fault_pid) != 0) {
            serial_write("[kernel] ring3 fault spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_write_probe(init_pid, &ring3_fault_write_pid) != 0) {
            serial_write("[kernel] ring3 fault write spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_exec_probe(init_pid, &ring3_fault_exec_pid) != 0) {
            serial_write("[kernel] ring3 fault exec spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_ud_probe(init_pid, &ring3_fault_ud_pid) != 0) {
            serial_write("[kernel] ring3 fault ud spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_gp_probe(init_pid, &ring3_fault_gp_pid) != 0) {
            serial_write("[kernel] ring3 fault gp spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_de_probe(init_pid, &ring3_fault_de_pid) != 0) {
            serial_write("[kernel] ring3 fault de spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_db_probe(init_pid, &ring3_fault_db_pid) != 0) {
            serial_write("[kernel] ring3 fault db spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_bp_probe(init_pid, &ring3_fault_bp_pid) != 0) {
            serial_write("[kernel] ring3 fault bp spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_of_probe(init_pid, &ring3_fault_of_pid) != 0) {
            serial_write("[kernel] ring3 fault of spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_nm_probe(init_pid, &ring3_fault_nm_pid) != 0) {
            serial_write("[kernel] ring3 fault nm spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_ss_probe(init_pid, &ring3_fault_ss_pid) != 0) {
            serial_write("[kernel] ring3 fault ss spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (kernel_ring3_spawn_fault_ac_probe(init_pid, &ring3_fault_ac_pid) != 0) {
            serial_write("[kernel] ring3 fault ac spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        ring3_fault_policy_probes_t fault_probes = {
            .fault_pid = ring3_fault_pid,
            .fault_write_pid = ring3_fault_write_pid,
            .fault_exec_pid = ring3_fault_exec_pid,
            .fault_ud_pid = ring3_fault_ud_pid,
            .fault_gp_pid = ring3_fault_gp_pid,
            .fault_de_pid = ring3_fault_de_pid,
            .fault_db_pid = ring3_fault_db_pid,
            .fault_bp_pid = ring3_fault_bp_pid,
            .fault_of_pid = ring3_fault_of_pid,
            .fault_nm_pid = ring3_fault_nm_pid,
            .fault_ss_pid = ring3_fault_ss_pid,
            .fault_ac_pid = ring3_fault_ac_pid
        };
        if (kernel_ring3_fault_policy_spawn(init_pid,
                                            &fault_probes,
                                            g_ring3_fault_churn_rounds,
                                            spawn_ring3_fault_churn_probe_process) != 0) {
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
    }
    run_low_slot_sweep_diagnostic();

    timer_init(250);
    serial_write("[kernel] interrupts on\n");
    cpu_enable_interrupts();

    serial_write("[kernel] scheduler loop\n");
    run_kernel_loop();
}
