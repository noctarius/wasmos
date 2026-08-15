/* kernel_boot_runtime.c - Boot-time helpers that outlive early bring-up.
 *
 * Three unrelated pieces the kernel entry path needs:
 *   - kernel_boot_build_bootinfo_shadow: re-homes the firmware's boot_info and
 *     its blobs into kernel-owned memory addressed through the higher-half
 *     alias, so nothing kernel-side depends on the low identity map.
 *   - kernel_boot_run_low_slot_sweep_diagnostic: strips and verifies the low
 *     slot of every live process root, logging (not panicking) on failure.
 *   - kernel_boot_run_scheduler_loop: the BSP's terminal scheduler loop, which
 *     panics only when not even the idle thread is dispatchable. */
#include "kernel_boot_runtime.h"

#include "paging.h"
#include "physmem.h"
#include "process.h"
#include "memory.h"
#include "klog.h"
#include "kpanic.h"
#include "string.h"
#include "timer.h"
#include "sched.h"

/* Allocate zeroed frames below 64 MiB and return the LOW (identity) pointer,
 * with the physical base in *out_phys.  The caller fills the buffer through
 * this low pointer, so this only works while the active root still has the low
 * slot — the kernel root keeps it (paging_init's bootstrap slot), process roots
 * do not.  Callers publish the higher-half alias, never this pointer. */
static void* boot_shadow_alloc_low(uint64_t size_bytes, uint64_t* out_phys) {
    const uint64_t page_size = 0x1000ULL;
    const uint64_t max_low = 64ULL * 1024ULL * 1024ULL;
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
 * caller panics. */
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
        __asm__ volatile("cli");
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
    }
}
