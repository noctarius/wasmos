/* warp/posix_kernel.c - Kernel-side stubs for the POSIX functions that
 * WARP's MemUtils layer calls when compiled with -DVB_POSIX.
 *
 * mmap/munmap: back JIT code pages with the kernel physical frame allocator.
 * Higher-half physical pages below 512 MB are already mapped RWX by the
 * kernel's initial page table; virt = phys | KERNEL_HIGHER_HALF_BASE.
 *
 * mprotect: no-op — JIT pages are RWX throughout their lifetime.  This is
 * acceptable for a kernel prototype; a hardened kernel would flip W^X.
 *
 * All other POSIX stubs (sigaction, sysconf, posix_memalign, close) are
 * trivial; the signal-based protection paths in WARP are dead because we
 * compile with ACTIVE_STACK_OVERFLOW_CHECK=1 and LINEAR_MEMORY_BOUNDS_CHECKS=1. */

#include <stddef.h>
#include <stdint.h>
#include "physmem.h"
#include "paging.h"
#include "slab.h"
#include "sys/mman.h"
#include "sys/signal.h"

/* 4 KiB kernel page size */
#define WARP_PAGE_SIZE  4096UL
#define WARP_MAX_MMAPS  64

typedef struct {
    uint8_t *virt;
    uint64_t phys;
    uint64_t pages;
} warp_mmap_entry_t;

static warp_mmap_entry_t g_mmaps[WARP_MAX_MMAPS];

static warp_mmap_entry_t *
find_mmap_slot(void)
{
    for (int i = 0; i < WARP_MAX_MMAPS; ++i) {
        if (!g_mmaps[i].virt) return &g_mmaps[i];
    }
    return NULL;
}

static warp_mmap_entry_t *
find_mmap_entry(uint8_t *virt)
{
    for (int i = 0; i < WARP_MAX_MMAPS; ++i) {
        if (g_mmaps[i].virt == virt) return &g_mmaps[i];
    }
    return NULL;
}

/* Round up to page boundary. */
static inline uint64_t
page_align(uint64_t n)
{
    return (n + WARP_PAGE_SIZE - 1) & ~(WARP_PAGE_SIZE - 1);
}

/* Kernel higher-half base — physical pages below 512 MB are already mapped
 * here as RWX large pages by the kernel's initial page table setup. */
#ifndef KERNEL_HIGHER_HALF_BASE
#define KERNEL_HIGHER_HALF_BASE 0xFFFFFFFF80000000ULL
#endif

/* 512 MB window — must stay below KERNEL_SHARED_HIGHER_HALF_WINDOW_BYTES */
#define WARP_JIT_PHYS_LIMIT (512ULL * 1024ULL * 1024ULL)

void *
mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    if (!length) return MAP_FAILED;

    uint64_t aligned = page_align((uint64_t)length);
    uint64_t pages   = aligned / WARP_PAGE_SIZE;

    uint64_t phys = pfa_alloc_pages_below(pages, WARP_JIT_PHYS_LIMIT);
    if (!phys) return MAP_FAILED;

    uint8_t *virt = (uint8_t *)(uintptr_t)(phys | KERNEL_HIGHER_HALF_BASE);

    warp_mmap_entry_t *slot = find_mmap_slot();
    if (!slot) {
        pfa_free_pages(phys, pages);
        return MAP_FAILED;
    }
    slot->virt  = virt;
    slot->phys  = phys;
    slot->pages = pages;

    return virt;
}

int
mprotect(void *addr, size_t len, int prot)
{
    /* No-op: JIT pages are RWX throughout their lifetime.
     * TODO: flip W^X when kernel paging supports per-4K granularity remapping. */
    (void)addr; (void)len; (void)prot;
    return 0;
}

int
munmap(void *addr, size_t length)
{
    (void)length;
    if (!addr || addr == MAP_FAILED) return -1;

    warp_mmap_entry_t *entry = find_mmap_entry((uint8_t *)addr);
    if (!entry) return -1;

    pfa_free_pages(entry->phys, entry->pages);
    entry->virt  = NULL;
    entry->phys  = 0;
    entry->pages = 0;
    return 0;
}

/* Return the kernel page size. */
long
sysconf(int name)
{
    (void)name; /* _SC_PAGE_SIZE = 30 on Linux, but we always return 4096. */
    return (long)WARP_PAGE_SIZE;
}

/* Aligned allocation — ignore alignment and use the slab allocator for now.
 * WARP calls posix_memalign only for aligned code buffers in MemUtils;
 * since we redirect ExecutableMemory to use mmap, this path is rarely taken. */
int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
    (void)alignment;
    if (!memptr) return 22; /* EINVAL */
    void *p = kalloc_small(size);
    if (!p) return 12; /* ENOMEM */
    *memptr = p;
    return 0;
}

/* Signal handler registration — no-op (signal-based protection is disabled
 * because ACTIVE_STACK_OVERFLOW_CHECK=1 and LINEAR_MEMORY_BOUNDS_CHECKS=1). */
int
sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    (void)sig; (void)act; (void)oldact;
    return 0;
}

int
close(int fd)
{
    (void)fd;
    return 0;
}

void
free(void *ptr)
{
    kfree_small(ptr);
}

void *
realloc(void *ptr, size_t size)
{
    /* Minimal realloc for MemUtils::alignedReduce — copy is conservative
     * (may over-copy) since we don't track the old block size here.
     * WARP only calls realloc to shrink aligned code buffers. */
    if (!ptr) return kalloc_small(size);
    if (!size) { kfree_small(ptr); return NULL; }
    void *n = kalloc_small(size);
    if (!n) return NULL;
    /* Conservatively copy at most `size` bytes (may read beyond old block
     * but in a kernel with trusted JIT code this is safe). */
    __builtin_memcpy(n, ptr, size);
    kfree_small(ptr);
    return n;
}
