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
#include "klog.h"
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
#include "kernel_ring3_smoke_runtime.h"
#include "kernel_ring3_suite_runtime.h"

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
    uint32_t idle_pid = 0;
    uint32_t init_pid = 0;

    (void)boot_info;

    serial_init();
    klog_write("[kernel] kmain\n");
    if (!boot_info || boot_info->version != BOOT_INFO_VERSION ||
        boot_info->size < sizeof(boot_info_t)) {
        klog_write("[kernel] invalid boot_info\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    klog_printf("[kernel] boot_info version=%016llx\n[kernel] boot_info size=%016llx\n",
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
        klog_write("[kernel] boot_info shadow copy failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    boot_info = &g_boot_info_shadow;
    g_boot_info = boot_info;
    ipc_init();
    process_init();
    wasm3_link_init(boot_info);

    klog_write("[kernel] wasm3 init on-demand\n");
    klog_write("[kernel] boot_info shadow active\n");

    if (process_spawn_idle("idle", idle_entry, 0, &idle_pid) != 0) {
        klog_write("[kernel] idle spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    kernel_init_state_reset(&g_init_state, boot_info);
    if (process_spawn("init", kernel_init_entry, &g_init_state, &init_pid) != 0) {
        klog_write("[kernel] init spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    klog_printf("[kernel] init pid=%016llx\n", (unsigned long long)init_pid);

    if (process_spawn_as(init_pid, "mem-service", memory_service_entry, 0, &mem_service_pid) != 0) {
        klog_write("[kernel] mem service spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    mem_service_proc = process_get(mem_service_pid);
    if (!mem_service_proc) {
        klog_write("[kernel] mem service lookup failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (ipc_endpoint_create(mem_service_proc->context_id, &mem_service_endpoint) != IPC_OK ||
        ipc_endpoint_create(IPC_CONTEXT_KERNEL, &mem_reply_endpoint) != IPC_OK) {
        klog_write("[kernel] mem service endpoint create failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    memory_service_register(mem_service_proc->context_id, mem_service_endpoint, mem_reply_endpoint);
    klog_write("[kernel] mem service ready\n");

    if (process_spawn_as(init_pid, "chardev-server", chardev_server_entry, 0, &chardev_pid) != 0) {
        klog_write("[kernel] chardev process spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    klog_printf("[kernel] chardev pid=%016llx\n", (unsigned long long)chardev_pid);

    chardev_proc = process_get(chardev_pid);
    if (!chardev_proc || wasm_chardev_init(chardev_proc->context_id) != 0) {
        klog_write("[kernel] chardev service init failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (wasm_chardev_endpoint(&chardev_endpoint) != 0) {
        klog_write("[kernel] chardev endpoint lookup failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    g_chardev_service_endpoint = chardev_endpoint;
    kernel_shmem_owner_isolation_test(mem_service_proc->context_id, chardev_proc->context_id);
    kernel_shmem_misuse_matrix_test(mem_service_proc->context_id,
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
        if (kernel_ring3_spawn_suite(init_pid,
                                     g_ring3_thread_lifecycle_smoke_enabled,
                                     g_ring3_fault_churn_rounds) != 0) {
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
    }
    run_low_slot_sweep_diagnostic();

    timer_init(250);
    klog_write("[kernel] interrupts on\n");
    cpu_enable_interrupts();

    klog_write("[kernel] scheduler loop\n");
    run_kernel_loop();
}
