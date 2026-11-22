/* device_window.c - kernel mappings of device register windows. See
 * device_window.h for the contract. */
#include "device_window.h"

#include "arch/vm_layout.h"
#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "physmem.h"
#include "sync/spinlock.h"

#define DEVICE_WINDOW_PAGE 0x1000ULL

typedef struct {
    uint64_t phys;
    uint64_t length;
    uint64_t kva;
} device_window_t;

/* Windows only ever accumulate: none is unmapped, so g_window_count and
 * g_window_next_va only grow. Both are written under g_window_lock. */
static device_window_t g_windows[DEVICE_WINDOW_LIMIT];
static uint32_t g_window_count;
static uint64_t g_window_next_va = KERNEL_MMIO_DEVICE_VA;
static ksync_spinlock_t g_window_lock;

static void device_window_unmap_pages(uint64_t va, uint64_t pages) {
    for (uint64_t i = 0; i < pages; ++i) {
        (void)paging_unmap_4k(va + i * DEVICE_WINDOW_PAGE);
        paging_invalidate(va + i * DEVICE_WINDOW_PAGE);
    }
}

int device_window_map(uint64_t phys, uint64_t length, uint64_t* out_kva) {
    uint64_t flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_DEVICE;
    uint64_t pages = length / DEVICE_WINDOW_PAGE;
    uint64_t va = 0;
    if (!out_kva || phys == 0u || length == 0u || length > DEVICE_WINDOW_MAX_BYTES ||
        (phys & (DEVICE_WINDOW_PAGE - 1u)) != 0u || (length & (DEVICE_WINDOW_PAGE - 1u)) != 0u ||
        phys + length < phys) {
        return -1;
    }
    if (pfa_range_overlaps_ram(phys, length)) {
        klog_printf("[device-window] refused %016llx+%llx: overlaps RAM\n",
                    (unsigned long long)phys,
                    (unsigned long long)length);
        return -1;
    }

    ksync_spinlock_lock(&g_window_lock);
    for (uint32_t i = 0; i < g_window_count; ++i) {
        const device_window_t* w = &g_windows[i];
        if (phys >= w->phys && phys + length <= w->phys + w->length) {
            *out_kva = w->kva + (phys - w->phys);
            ksync_spinlock_unlock(&g_window_lock);
            return 0;
        }
    }
    if (g_window_count >= DEVICE_WINDOW_LIMIT ||
        g_window_next_va + length > KERNEL_MMIO_DEVICE_VA + KERNEL_MMIO_DEVICE_BYTES) {
        ksync_spinlock_unlock(&g_window_lock);
        klog_printf("[device-window] refused %016llx+%llx: no window left\n",
                    (unsigned long long)phys,
                    (unsigned long long)length);
        return -1;
    }
    va = g_window_next_va;
    for (uint64_t i = 0; i < pages; ++i) {
        if (paging_map_4k(va + i * DEVICE_WINDOW_PAGE, phys + i * DEVICE_WINDOW_PAGE, flags) != 0) {
            device_window_unmap_pages(va, i);
            ksync_spinlock_unlock(&g_window_lock);
            return -1;
        }
        paging_invalidate(va + i * DEVICE_WINDOW_PAGE);
    }
    g_windows[g_window_count].phys = phys;
    g_windows[g_window_count].length = length;
    g_windows[g_window_count].kva = va;
    g_window_count++;
    g_window_next_va = va + length;
    ksync_spinlock_unlock(&g_window_lock);
    *out_kva = va;
    return 0;
}
