#include "kernel_boot_runtime.h"

#include "paging.h"
#include "physmem.h"
#include "process.h"
#include "memory.h"
#include "klog.h"
#include "string.h"
#include "timer.h"

static void *
boot_shadow_alloc_low(uint64_t size_bytes, uint64_t *out_phys)
{
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
    void *low = (void *)(uintptr_t)phys;
    memset(low, 0, (size_t)(pages * page_size));
    if (out_phys) {
        *out_phys = phys;
    }
    return low;
}

static int
boot_shadow_copy_blob(void **dst_ptr, const void *src_ptr, uint64_t size_bytes)
{
    if (!dst_ptr) {
        return -1;
    }
    *dst_ptr = 0;
    if (!src_ptr || size_bytes == 0) {
        return 0;
    }
    uint64_t dst_phys = 0;
    void *dst_low = boot_shadow_alloc_low(size_bytes, &dst_phys);
    if (!dst_low) {
        return -1;
    }
    memcpy(dst_low, src_ptr, (size_t)size_bytes);
    *dst_ptr = (void *)(uintptr_t)(dst_phys + KERNEL_HIGHER_HALF_BASE);
    return 0;
}

int
kernel_boot_build_bootinfo_shadow(const boot_info_t *src, boot_info_t *dst)
{
    if (!src || !dst) {
        return -1;
    }
    memcpy(dst, src, sizeof(*dst));
    if (boot_shadow_copy_blob(&dst->rsdp, src->rsdp, (uint64_t)src->rsdp_length) != 0) {
        return -1;
    }
    if (boot_shadow_copy_blob(&dst->boot_config, src->boot_config, (uint64_t)src->boot_config_size) != 0) {
        return -1;
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
    void *table_low = boot_shadow_alloc_low(table_size, &table_phys);
    if (!table_low) {
        return -1;
    }
    memcpy(table_low, src->modules, (size_t)table_size);
    dst->modules = (void *)(uintptr_t)(table_phys + KERNEL_HIGHER_HALF_BASE);

    uint8_t *mods_low = (uint8_t *)table_low;
    for (uint32_t i = 0; i < src->module_count; ++i) {
        boot_module_t *mod = (boot_module_t *)(mods_low + (uint64_t)i * (uint64_t)src->module_entry_size);
        if (!mod || mod->base == 0 || mod->size == 0) {
            continue;
        }
        if (mod->size > 0xFFFFFFFFULL) {
            return -1;
        }
        void *shadow_blob_high = 0;
        const void *blob_src = (const void *)(uintptr_t)mod->base;
        if (boot_shadow_copy_blob(&shadow_blob_high, blob_src, mod->size) != 0) {
            return -1;
        }
        mod->base = (uint64_t)(uintptr_t)shadow_blob_high;
    }
    return 0;
}

void
kernel_boot_run_low_slot_sweep_diagnostic(void)
{
    uint32_t active = process_count_active();
    uint32_t pid = 0;
    uint32_t parent_pid = 0;
    const char *name = 0;
    uint8_t failed = 0;

    klog_write("[diag] low-slot sweep start\n");
    for (uint32_t i = 0; i < active; ++i) {
        if (process_info_at_ex(i, &pid, &parent_pid, &name) != 0) {
            continue;
        }
        process_t *proc = process_get(pid);
        if (!proc || proc->is_idle || proc->context_id == 0) {
            continue;
        }
        uint64_t root = mm_context_root_table(proc->context_id);
        if (root == 0) {
            continue;
        }
        if (paging_strip_low_slot_in_root(root) != 0) {
            klog_printf("[diag] low-slot sweep fail: strip pid=%u name=%s ctx=%u root=%016llx\n",
                          pid, name ? name : "(null)", proc->context_id, (unsigned long long)root);
            failed = 1;
            break;
        }
        if (paging_verify_user_root_no_low_slot(root, 1) != 0) {
            klog_printf("[diag] low-slot sweep fail: verify pid=%u name=%s ctx=%u root=%016llx\n",
                          pid, name ? name : "(null)", proc->context_id, (unsigned long long)root);
            failed = 1;
            break;
        }
    }
    if (!failed) {
        klog_write("[diag] low-slot sweep ok\n");
    }
}

void
kernel_boot_run_scheduler_loop(void)
{
    for (;;) {
        __asm__ volatile("cli");
        if (process_schedule_once() != 0) {
            __asm__ volatile("pause");
        }
        if (process_should_resched()) {
            process_clear_resched();
        }
        timer_poll();
    }
}
