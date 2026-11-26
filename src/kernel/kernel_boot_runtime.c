/* kernel_boot_runtime.c - Boot-time helpers that outlive early bring-up.
 *
 * Three unrelated pieces the kernel entry path needs:
 *   - kernel_boot_build_bootinfo_shadow: re-homes the firmware's boot_info and
 *     its blobs into kernel-owned memory addressed through the higher-half
 *     alias, so nothing kernel-side depends on the low identity map.
 *   - kernel_boot_run_low_slot_sweep_diagnostic: strips and verifies the low
 *     slot of every live process root, logging (not panicking) on failure.
 *   - kernel_boot_run_scheduler_loop: the BSP's terminal scheduler loop, which
 *     panics only when not even the idle thread is dispatchable.
 *   - kernel_boot_kpanic_stop_test: the debug.kpanic_stop_test boot option,
 *     which panics with a CPU spinning where it takes no ordinary interrupt,
 *     so a test can see whether kpanic's stop reached it.
 *   - kernel_boot_exception_test: the debug.exception_test boot option, which
 *     raises a CPU exception in ring 0 from a known rip and rsp, so a test can
 *     check what the exception dump reports for them.
 *   - kernel_boot_leak_test: the debug.leak_test boot option, which builds and
 *     tears down address spaces (and WARP ring-3 setups) repeatedly and logs
 *     free physical memory before and after, so a test can see a leak. */
#include "kernel_boot_runtime.h"

#include "arch/arch_cpu.h"
#include "paging.h"
#include "physmem.h"
#include "process.h"
#include "memory.h"
#include "klog.h"
#include "kpanic.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "sched.h"
#include "cpu_local.h"
#include "kenv.h"
#include "wasmos_status.h"
#if !defined(WASMOS_ARCH_AARCH64)
#include "arch/x86_64/cpu_x86_64.h"
#endif
#if defined(WASMOS_WASM_RUNTIME_WARP) && !defined(WASMOS_ARCH_AARCH64)
#include "warp_ring3.h"
#endif

/* debug.kpanic_stop_test: the boot CPU arms the test with a mode, CPU 1 starts
 * spinning and announces it, and the boot CPU then panics. kpanic's stop has
 * to reach a CPU that takes no ordinary interrupt for its frame to appear in
 * the dump:
 *   1  CPU 1 spins with interrupts masked, from its scheduler loop;
 *   2  CPU 1 spins inside its timer interrupt, before ending it, so the
 *      interrupt controller holds it at that interrupt's priority. */
#define KPANIC_STOP_TEST_MASKED 1u
#define KPANIC_STOP_TEST_IN_IRQ 2u

#if WASMOS_SMP
static uint32_t g_kpanic_stop_test_armed;
static uint32_t g_kpanic_stop_test_spinning;

static __attribute__((noreturn)) void kpanic_stop_test_spin(void) {
    arch_irq_disable();
    __atomic_store_n(&g_kpanic_stop_test_spinning, 1u, __ATOMIC_RELEASE);
    for (;;) {
        arch_cpu_relax();
    }
}
#endif

void kernel_boot_kpanic_stop_test_tick(void) {
#if WASMOS_SMP
    if (__atomic_load_n(&g_kpanic_stop_test_armed, __ATOMIC_RELAXED) == KPANIC_STOP_TEST_IN_IRQ &&
        cpu_local()->cpu_id == 1u) {
        kpanic_stop_test_spin();
    }
#endif
}

void kernel_boot_kpanic_stop_test(void) {
    char value[4];
    uint32_t written = 0;
    if (kenv_get("debug.kpanic_stop_test", value, sizeof(value), &written) != WASMOS_OK) {
        return;
    }
    uint32_t mode = 0;
    if (strcmp(value, "1") == 0) {
        mode = KPANIC_STOP_TEST_MASKED;
    } else if (strcmp(value, "2") == 0) {
        mode = KPANIC_STOP_TEST_IN_IRQ;
    } else {
        klog_printf("[kpanic-stop-test] mode %s unknown; not run\n", value);
        return;
    }
#if WASMOS_SMP
    if (g_cpu_count < 2u || !__atomic_load_n(&g_cpus[1].started, __ATOMIC_ACQUIRE)) {
        klog_write("[kpanic-stop-test] needs a second online CPU; not run\n");
        return;
    }
    __atomic_store_n(&g_kpanic_stop_test_armed, mode, __ATOMIC_RELEASE);
    /* CPU 1 reaches its scheduler loop within a tick; the bound only keeps a
     * CPU that never does from hanging the boot, and the panic then reports it
     * as it is. */
    for (uint64_t spin = 0; spin < 2000000000ULL; ++spin) {
        if (__atomic_load_n(&g_kpanic_stop_test_spinning, __ATOMIC_ACQUIRE)) {
            break;
        }
        arch_cpu_relax();
    }
    klog_printf("[kpanic-stop-test] cpu 1 spinning (mode %u): %u\n",
                (unsigned)mode,
                (unsigned)__atomic_load_n(&g_kpanic_stop_test_spinning, __ATOMIC_ACQUIRE));
    kpanic("kpanic stop test", 1, 0);
#else
    (void)mode;
    klog_write("[kpanic-stop-test] needs an SMP kernel; not run\n");
#endif
}

void kernel_boot_exception_test(void) {
    char value[4];
    uint32_t written = 0;
    if (kenv_get("debug.exception_test", value, sizeof(value), &written) != WASMOS_OK) {
        return;
    }
    uint32_t vector = 0;
    for (const char* c = value; *c; ++c) {
        if (*c < '0' || *c > '9') {
            klog_printf("[exception-test] vector %s unknown; not run\n", value);
            return;
        }
        vector = vector * 10u + (uint32_t)(*c - '0');
    }
#if defined(WASMOS_ARCH_AARCH64)
    (void)vector;
    klog_write("[exception-test] needs x86_64; not run\n");
#else
    x86_exception_test_raise(vector);
#endif
}

/* debug.leak_test: each cycle builds one object and releases it, so free physical
 * memory must come back to the same byte count however many cycles run.  One
 * cycle runs first, unmeasured, so any one-time growth it triggers is not counted
 * against the rest. */
#define LEAK_TEST_CYCLES 16u

/* An address space with a private copy of the low slot, as every process root
 * starts; `strip` also removes the slot again first, as process roots then do. */
static int leak_test_address_space(int strip) {
    uint64_t root = 0;
    if (paging_create_address_space(&root) != 0) {
        return -1;
    }
    if (paging_clone_low_slot_in_root(root) != 0) {
        paging_destroy_address_space(root);
        return -1;
    }
    if (strip) {
        (void)paging_strip_low_slot_in_root(root);
    }
    paging_destroy_address_space(root);
    return 0;
}

static int leak_test_cloned(void) {
    return leak_test_address_space(0);
}

static int leak_test_stripped(void) {
    return leak_test_address_space(1);
}

#if defined(WASMOS_WASM_RUNTIME_WARP) && !defined(WASMOS_ARCH_AARCH64)
static int leak_test_warp_r3(void) {
    uint64_t root = 0;
    uint64_t stack = 0;
    if (warp_r3_setup(&root, &stack) != 0) {
        return -1;
    }
    warp_r3_teardown(root, stack);
    return 0;
}
#endif

static void leak_test_run(const char* what, int (*cycle)(void)) {
    if (cycle() != 0) {
        klog_printf("[leak-test] %s: setup failed; not run\n", what);
        return;
    }
    uint64_t before = pfa_free_bytes();
    for (uint32_t i = 0; i < LEAK_TEST_CYCLES; ++i) {
        if (cycle() != 0) {
            klog_printf("[leak-test] %s: setup failed; not run\n", what);
            return;
        }
    }
    uint64_t after = pfa_free_bytes();
    klog_printf("[leak-test] %s: cycles=%u before=%llu after=%llu\n",
                what,
                (unsigned)LEAK_TEST_CYCLES,
                (unsigned long long)before,
                (unsigned long long)after);
}

void kernel_boot_leak_test(void) {
    char value[4];
    uint32_t written = 0;
    if (kenv_get("debug.leak_test", value, sizeof(value), &written) != WASMOS_OK) {
        return;
    }
    leak_test_run("address space with a cloned low slot", leak_test_cloned);
    leak_test_run("address space with the low slot stripped", leak_test_stripped);
#if defined(WASMOS_WASM_RUNTIME_WARP) && !defined(WASMOS_ARCH_AARCH64)
    leak_test_run("warp ring-3 setup", leak_test_warp_r3);
#else
    klog_write("[leak-test] warp ring-3 setup: needs an x86_64 WARP kernel; not run\n");
#endif
    klog_write("[leak-test] done\n");
}

/* Allocate zeroed frames inside the bootstrap window (KERNEL_BOOTSTRAP_WINDOW_BYTES,
 * arch/vm_layout.h: 64 MiB on x86_64, the whole direct map on aarch64, whose RAM
 * starts at 1 GiB) and return the LOW (identity) pointer, with the physical base
 * in *out_phys.  The caller fills the buffer through this low pointer, so this
 * only works while the active root still has the low slot — the kernel root
 * keeps it (paging_init's bootstrap slot), process roots do not.  Callers
 * publish the higher-half alias, never this pointer. */
static void* boot_shadow_alloc_low(uint64_t size_bytes, uint64_t* out_phys) {
    const uint64_t page_size = 0x1000ULL;
    const uint64_t max_low = KERNEL_BOOTSTRAP_WINDOW_BYTES;
    if (size_bytes == 0) {
        return 0;
    }
    uint64_t pages = (size_bytes + page_size - 1ULL) / page_size;
    if (pages == 0) {
        return 0;
    }
    uint64_t phys = pfa_alloc_pages_below(pages, max_low);
    if (!phys) {
        return 0;
    }
    void* low = ptr_cast(void, phys);
    memset(low, 0, (size_t)(pages * page_size));
    if (out_phys) {
        *out_phys = phys;
    }
    return low;
}

static int boot_shadow_copy_blob(void** dst_ptr, const void* src_ptr, uint64_t size_bytes) {
    if (!dst_ptr) {
        return -1;
    }
    *dst_ptr = 0;
    if (!src_ptr || size_bytes == 0) {
        return 0;
    }
    uint64_t dst_phys = 0;
    void* dst_low = boot_shadow_alloc_low(size_bytes, &dst_phys);
    if (!dst_low) {
        return -1;
    }
    memcpy(dst_low, src_ptr, (size_t)size_bytes);
    *dst_ptr = ptr_cast(void, (dst_phys + KERNEL_HIGHER_HALF_BASE));
    return 0;
}

/* Builds a kernel-owned copy of boot_info in *dst, with each referenced blob
 * (RSDP, boot config, initfs modules) re-homed into freshly allocated frames and
 * every pointer in *dst rewritten to that blob's higher-half alias.
 *
 * Both the structure and the blobs are read from src through the LOW identity
 * map, so this must run while the kernel root still has its low slot and after
 * the frame allocator is up.  src is borrowed and can be discarded afterwards;
 * dst must outlive the kernel, since nothing copies it again.
 *
 * Returns 0 on success and -1 for a NULL argument or when any blob's frames
 * cannot be allocated.  A partial failure leaves *dst half-rewritten and the
 * frames already taken are not released, so a -1 is not recoverable and the
 * caller panics.
 *
 * FIXME(boot-ram-above-window): on x86_64 a guest with more than about 512 MiB of RAM
 * faults in the memcpy here, reading boot data the firmware placed outside the 512 MiB
 * higher-half window (seen with -m 2G at 0x7db23000; docs/TASKS.md). */
int kernel_boot_build_bootinfo_shadow(const boot_info_t* src, boot_info_t* dst) {
    if (!src || !dst) {
        return -1;
    }
    memcpy(dst, src, sizeof(*dst));
    if (boot_shadow_copy_blob(&dst->rsdp, src->rsdp, (uint64_t)src->rsdp_length) != 0) {
        return -1;
    }
    if (boot_shadow_copy_blob(
            &dst->boot_config, src->boot_config, (uint64_t)src->boot_config_size) != 0) {
        return -1;
    }
    /* Remap initfs to its higher-half virtual alias.  UEFI allocates the initfs
     * at a physical address, and the shared higher-half window already covers
     * the first HIGHER_HALF_PDE_COUNT * 2 MiB (512 MiB) of physical RAM, so a
     * pointer fixup is enough — no data copy.  Without it every kernel caller
     * dereferencing boot_info->initfs would need the low identity mapping,
     * which the boot-time sweep strips from process page tables.
     * TODO: an initfs the firmware placed above that window is not covered by
     * the alias and this fixup silently produces an unmapped pointer. */
    if ((src->flags & BOOT_INFO_FLAG_INITFS_PRESENT) && src->initfs && src->initfs_size > 0) {
        uint64_t phys = addr_cast(uint64_t, dst->initfs);
        if (phys < KERNEL_HIGHER_HALF_BASE) {
            dst->initfs = ptr_cast(void, (phys + KERNEL_HIGHER_HALF_BASE));
        }
    }
    if (!(src->flags & BOOT_INFO_FLAG_MODULES_PRESENT) || !src->modules || src->module_count == 0 ||
        src->module_entry_size < sizeof(boot_module_t)) {
        return 0;
    }

    uint64_t table_size = (uint64_t)src->module_count * (uint64_t)src->module_entry_size;
    if (table_size == 0 || table_size > 0xFFFFFFFFULL) {
        return -1;
    }

    uint64_t table_phys = 0;
    void* table_low = boot_shadow_alloc_low(table_size, &table_phys);
    if (!table_low) {
        return -1;
    }
    memcpy(table_low, src->modules, (size_t)table_size);
    dst->modules = ptr_cast(void, (table_phys + KERNEL_HIGHER_HALF_BASE));

    uint8_t* mods_low = (uint8_t*)table_low;
    for (uint32_t i = 0; i < src->module_count; ++i) {
        boot_module_t* mod =
            (boot_module_t*)(mods_low + (uint64_t)i * (uint64_t)src->module_entry_size);
        if (!mod || mod->base == 0 || mod->size == 0) {
            continue;
        }
        if (mod->size > 0xFFFFFFFFULL) {
            return -1;
        }
        void* shadow_blob_high = 0;
        const void* blob_src = ptr_cast(void, mod->base);
        if (boot_shadow_copy_blob(&shadow_blob_high, blob_src, mod->size) != 0) {
            return -1;
        }
        mod->base = addr_cast(uint64_t, shadow_blob_high);
    }
    return 0;
}

/* Strips the low identity slot from every live, non-idle process root and
 * verifies the result, as a one-shot boot-time check that no process is left
 * with a mapping into low memory.
 *
 * It MUTATES the roots it visits — this is not a read-only diagnostic — but
 * reports only through klog and returns nothing, so a caller cannot tell success
 * from failure.  The sweep stops at the first failure rather than continuing, so
 * processes after it keep their low slot.  Idle processes and context 0 are
 * skipped by design; the kernel root keeps its low slot. */
void kernel_boot_run_low_slot_sweep_diagnostic(void) {
    uint32_t active = process_count_active();
    uint32_t pid = 0;
    uint32_t parent_pid = 0;
    const char* name = 0;
    uint8_t failed = 0;

    klog_write("[diag] low-slot sweep start\n");
    for (uint32_t i = 0; i < active; ++i) {
        if (process_info_at_ex(i, &pid, &parent_pid, &name) != 0) {
            continue;
        }
        process_t* proc = process_get(pid);
        if (!proc || proc->is_idle || proc->context_id == 0) {
            continue;
        }
        uint64_t root = mm_context_root_table(proc->context_id);
        if (root == 0) {
            continue;
        }
        if (paging_strip_low_slot_in_root(root) != 0) {
            klog_printf("[diag] low-slot sweep fail: strip pid=%u name=%s ctx=%u root=%016llx\n",
                        pid,
                        name ? name : "(null)",
                        proc->context_id,
                        (unsigned long long)root);
            failed = 1;
            break;
        }
        if (paging_verify_user_root_no_low_slot(root, 1) != 0) {
            klog_printf("[diag] low-slot sweep fail: verify pid=%u name=%s ctx=%u root=%016llx\n",
                        pid,
                        name ? name : "(null)",
                        proc->context_id,
                        (unsigned long long)root);
            failed = 1;
            break;
        }
    }
    if (!failed) {
        klog_write("[diag] low-slot sweep ok\n");
    }
}

/* The BSP's terminal loop: it never returns, and the only exit is the panic
 * below.
 *
 * Each iteration masks interrupts, dispatches one thread, then services the
 * reschedule flag and the timer.  It does NOT idle: a CPU with nothing to run
 * dispatches the idle thread, which is what executes sti;hlt, so the loop itself
 * spins only between dispatches.
 *
 * Transient scheduler results — including SCHED_R_STALE, a thread reaped from
 * under the picker — simply re-loop.  SCHED_R_PICK, SCHED_R_CTX, SCHED_R_ROOT
 * and SCHED_R_MAXCOUNT mean the invariant "idle is always dispatchable" has been
 * broken, and panic. */
void kernel_boot_run_scheduler_loop(void) {
    for (;;) {
        arch_irq_disable();
#if WASMOS_SMP
        if (__atomic_load_n(&g_kpanic_stop_test_armed, __ATOMIC_RELAXED) ==
                KPANIC_STOP_TEST_MASKED &&
            cpu_local()->cpu_id == 1u) {
            kpanic_stop_test_spin();
        }
#endif
        int rc = process_schedule_once();
        /* SCHED_OK, or a thread that just blocked/exited/zombied/raced
         * (including SCHED_R_STALE, a thread reaped out from under the picker),
         * re-loops immediately — the idle thread does the actual (sti;hlt)
         * idling.  SCHED_R_PICK means "not even idle was dispatchable", a real
         * invariant violation, which is what the panic below reports. */
        if (rc == SCHED_R_PICK || rc == SCHED_R_CTX || rc == SCHED_R_ROOT || rc == SCHED_R_MAXCOUNT)
            kpanic("scheduler: no runnable thread (idle not dispatchable)", (uint64_t)rc, 0);
        if (process_should_resched())
            process_clear_resched();
        timer_poll();
        /* Enqueue anything a wake could not link because the thread was still
         * executing.  Here rather than in the picker, which runs under the
         * queue lock this would take, and here rather than only at dispatch,
         * because the ordering it exists to cover is a claim published just
         * after the dispatching CPU looked for one.  One load when nothing is
         * owed. */
        sched_sweep_owed_enqueues();
        /* Deferred klog doorbell: the logging path only raises a flag, because it
         * can run under the serial lock, in interrupt context and during a panic.
         * This is the first point after a dispatch where sending IPC is safe. */
        klog_poll();
        kmsg_poll();
    }
}
