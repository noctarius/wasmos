/* memory.c - Virtual memory context management and shared region allocator.
 * Each process has an mm_context_t with a list of mem_region_t entries and a PML4.
 * mm_handle_page_fault() demand-maps pages on first access.
 * mm_shared_* implements cross-process shared memory (used for DMA and framebuffer). */
#include "memory.h"
#include "kpanic.h"
#include "klog.h"
#include "paging.h"
#include "physmem.h"
#include "serial.h"
#include "list.h"
#include "sync/spinlock.h"
#include "string.h"
#include "arch/x86_64/smp.h"

#define PAGE_SIZE 0x1000ULL
#define MM_USER_LINEAR_BASE 0x0000008000000000ULL
#define MM_USER_STACK_BASE 0x0000008100000000ULL
#define MM_USER_HEAP_BASE 0x0000008200000000ULL
#define MM_USER_IPC_BASE 0x0000008300000000ULL
#define MM_USER_DEVICE_BASE 0x0000008400000000ULL
#define MM_USER_SHARED_BASE 0x0000008500000000ULL
#define MM_COPY_STACK_BYTES 8192u
static const uint64_t pf_err_present = 1ULL << 0;
static const uint64_t pf_err_write = 1ULL << 1;
static const uint64_t pf_err_user = 1ULL << 2;
static const uint64_t pf_err_instr = 1ULL << 4;
static const boot_info_t* g_boot_info;
static list_t g_contexts;
static ksync_spinlock_t g_contexts_lock;
static mm_context_t g_root_ctx;
static uint8_t g_mm_copy_stacks[WASMOS_MAX_CPUS][MM_COPY_STACK_BYTES] __attribute__((aligned(16)));

/* Kernel higher-half alias of a low (identity-mapped) address; already-high
 * addresses pass through unchanged. */
static inline uintptr_t mm_kernel_alias_addr(uintptr_t addr) {
    if ((uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        return (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return addr;
}

/* Array-chunk capacity of g_shared_list, not a hard cap: the list grows by
 * whole chunks. It also bounds the id-probe loop in mm_shared_create. */
#define MM_MAX_SHARED 16
/* Grant slots per shared region; grants beyond this are refused. */
#define MM_MAX_SHARED_GRANTS 8
typedef struct {
    uint32_t id;
    uint32_t owner_context_id;
    uint32_t refcount;
    uint64_t base;
    uint64_t pages;
    uint32_t flags;
    uint32_t grant_contexts[MM_MAX_SHARED_GRANTS];
    uint8_t grant_count;
} mm_shared_region_t;

static list_t g_shared_list;
static uint8_t g_shared_list_initialized = 0;
static uint32_t g_shared_next_id = 1;
static ksync_spinlock_t g_shared_lock;

static void mm_shared_init_once_locked(void) {
    if (g_shared_list_initialized) {
        return;
    }
    list_init(&g_shared_list, (uint32_t)sizeof(mm_shared_region_t), LIST_IMPL_ARRAY_CHUNK,
              MM_MAX_SHARED);
    g_shared_list_initialized = 1;
}
static int mm_region_flags_valid(uint32_t flags);
typedef int (*mm_copy_work_fn)(void* arg);
static mem_region_t* mm_context_add_region_slot(mm_context_t* ctx, uint64_t base, uint64_t size,
                                                uint32_t flags, mem_region_type_t type);
static void mm_context_release_regions(mm_context_t* ctx);
static mm_context_t* mm_context_get_locked(uint32_t id);
static mm_shared_region_t* mm_shared_find_locked(uint32_t id);

/* Run fn(arg) on this CPU's higher-half copy stack.  The user-copy helpers flip
 * CR3 to a process root, which carries no low identity mapping, so a stack that
 * currently lives at a low VA would disappear mid-copy.  A stack already in the
 * higher half is safe under any root and is used as-is.  Returns fn's value, or
 * -1 for a NULL fn (internal-only; never crosses a subsystem boundary). */
static int mm_run_on_copy_stack(mm_copy_work_fn fn, void* arg) {
    if (!fn) {
        return -1;
    }

    uint64_t rsp = 0;
    uint64_t higher_half_base = paging_get_higher_half_base();
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    if (rsp >= higher_half_base) {
        return fn(arg);
    }

    uintptr_t stack_top = mm_kernel_alias_addr(
        (uintptr_t)&g_mm_copy_stacks[cpu_local()->cpu_id][MM_COPY_STACK_BYTES]);
    stack_top &= ~(uintptr_t)0xFULL;
    int rc = -1;
    __asm__ volatile("mov %%rsp, %%r15\n"
                     "mov %[stack_top], %%rsp\n"
                     "mov %[arg], %%rdi\n"
                     "call *%[fn]\n"
                     "mov %%r15, %%rsp\n"
                     : "=a"(rc)
                     : [stack_top] "r"(stack_top), [fn] "r"(fn), [arg] "r"(arg)
                     : "r15", "rdi", "rcx", "rdx", "rsi", "r8", "r9", "r10", "memory", "cc");
    return rc;
}

static uint64_t mm_region_virtual_base(mm_context_t* ctx, mem_region_type_t type, uint64_t pages) {
    (void)pages;
    if (!ctx) {
        return 0;
    }
    switch (type) {
    case MEM_REGION_WASM_LINEAR:
        return MM_USER_LINEAR_BASE;
    case MEM_REGION_STACK:
        return MM_USER_STACK_BASE;
    case MEM_REGION_HEAP:
        return MM_USER_HEAP_BASE;
    case MEM_REGION_IPC:
        return MM_USER_IPC_BASE;
    case MEM_REGION_DEVICE:
        return MM_USER_DEVICE_BASE;
    case MEM_REGION_SHARED: {
        uint64_t base = ctx->next_shared_base;
        ctx->next_shared_base += pages * PAGE_SIZE;
        return base;
    }
    case MEM_REGION_CODE:
    default:
        return 0;
    }
}

/* Brings the whole memory subsystem up in order: locks, the frame allocator, the
 * kernel page tables, the context list, and finally context 0 with its default
 * regions.  Runs once on the BSP before any other mm_* call.
 *
 * boot_info is retained as a borrowed pointer for the life of the kernel, so it
 * must reference memory that survives ExitBootServices.
 *
 * Nothing here is fatal by itself — a failed paging_init, context-list init or
 * region allocation is logged and execution continues — so a caller that needs a
 * working subsystem has to check the state it depends on.  Context 0's regions
 * are created WITHOUT MEM_REGION_FLAG_USER: it is the kernel/supervisor context
 * and its regions are not reachable from ring 3. */
void mm_init(const boot_info_t* boot_info) {
    g_boot_info = boot_info;
    klog_write("[mm] init\n");
    ksync_spinlock_init(&g_contexts_lock);
    ksync_spinlock_init(&g_shared_lock);
    pfa_init(boot_info);
    if (paging_init() != 0) {
        klog_write("[mm] paging init failed\n");
    } else {
        klog_write("[mm] paging init\n");
    }

    if (list_init(&g_contexts, (uint32_t)sizeof(mm_context_t), LIST_IMPL_ARRAY_CHUNK, 16) != 0) {
        klog_write("[mm] context list init failed\n");
    }

    if (mm_context_init(&g_root_ctx, 0) == 0) {
        g_root_ctx.root_table = paging_get_root_table();
        mm_context_alloc_region(&g_root_ctx, 16, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                                MEM_REGION_WASM_LINEAR);
        mm_context_alloc_region(&g_root_ctx, 4, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                                MEM_REGION_STACK);
        mm_context_alloc_region(&g_root_ctx, 8, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                                MEM_REGION_HEAP);
        mm_context_alloc_region(&g_root_ctx, 2, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                                MEM_REGION_IPC);
        mm_context_alloc_region(&g_root_ctx, 2, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                                MEM_REGION_DEVICE);
    }

    klog_printf("[mm] ctx0 regions=0x%016llX\n", (unsigned long long)g_root_ctx.region_count);
}

/* Zero-initialises a caller-provided context and creates its region list.  It
 * does NOT create an address space: root_table is left 0 and must be filled in
 * by the caller (mm_context_create does it through paging_create_address_space).
 *
 * Returns 0 on success, -1 for a NULL ctx or a failed list_init.  ctx is fully
 * overwritten, so calling this on a live context leaks its regions and its
 * region list. */
int mm_context_init(mm_context_t* ctx, uint32_t id) {
    if (!ctx) {
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->id = id;
    ctx->root_table = 0;
    ctx->next_shared_base = MM_USER_SHARED_BASE;
    ctx->region_count = 0;
    if (list_init(&ctx->regions, (uint32_t)sizeof(mem_region_t), LIST_IMPL_ARRAY_CHUNK, 8) != 0) {
        return -1;
    }
    return 0;
}

/* Records a region descriptor only: `base` is a virtual address in ctx's address
 * space and nothing is allocated, mapped, or given a physical backing —
 * phys_base and backing_pages stay 0, so the region owns no frames and
 * mm_context_release_regions will not free any for it.  Use
 * mm_context_alloc_region for a region that needs backing.
 *
 * Returns 0 on success, -1 for a NULL ctx, a W+X user flag combination, or a
 * full region list. */
int mm_context_add_region(mm_context_t* ctx, uint64_t base, uint64_t size, uint32_t flags,
                          mem_region_type_t type) {
    return mm_context_add_region_slot(ctx, base, size, flags, type) ? 0 : -1;
}

/* Copies the FIRST region of the given type into *out_region by value, so the
 * snapshot does not track later changes and cannot be used to mutate the
 * region.  Returns 0 on a hit, -1 for a NULL argument or no region of that type.
 * MEM_REGION_SHARED can occur many times in one context; this returns whichever
 * the list yields first. */
int mm_context_region_for_type(mm_context_t* ctx, mem_region_type_t type,
                               mem_region_t* out_region) {
    if (!ctx || !out_region) {
        return -1;
    }
    list_iter_t it;
    mem_region_t* region = (mem_region_t*)list_first(&ctx->regions, &it);
    while (region) {
        if (region->type == type) {
            *out_region = *region;
            return 0;
        }
        region = (mem_region_t*)list_next(&it);
    }
    return -1;
}

/* Copies the region at a zero-based iteration position into *out_region.  The
 * position is the list's current traversal order, not a stable handle: adding or
 * removing any region can renumber the rest, so an enumeration must not be
 * interleaved with mutation.  Returns 0 on a hit, -1 for a NULL argument or an
 * index past the end. */
int mm_context_region_at(mm_context_t* ctx, uint32_t index, mem_region_t* out_region) {
    uint32_t current = 0;
    list_iter_t it;
    mem_region_t* region = 0;
    if (!ctx || !out_region) {
        return -1;
    }
    region = (mem_region_t*)list_first(&ctx->regions, &it);
    while (region) {
        if (current == index) {
            *out_region = *region;
            return 0;
        }
        current++;
        region = (mem_region_t*)list_next(&it);
    }
    return -1;
}

static mm_shared_region_t* mm_shared_find_locked(uint32_t id) {
    if (id == 0) {
        return 0;
    }
    mm_shared_init_once_locked();
    list_iter_t it;
    mm_shared_region_t* r = (mm_shared_region_t*)list_first(&g_shared_list, &it);
    while (r) {
        if (r->id == id) {
            return r;
        }
        r = (mm_shared_region_t*)list_next(&it);
    }
    return 0;
}

static int mm_shared_access_allowed(const mm_shared_region_t* region, uint32_t context_id) {
    uint8_t i = 0;
    if (!region) {
        return 0;
    }
    /* Context 0 is kernel/supervisor and may inspect or manage any region. */
    if (context_id == 0) {
        return 1;
    }
    if (region->owner_context_id == context_id) {
        return 1;
    }
    for (i = 0; i < region->grant_count && i < MM_MAX_SHARED_GRANTS; ++i) {
        if (region->grant_contexts[i] == context_id) {
            return 1;
        }
    }
    return 0;
}

static int mm_shared_free_if_unused(mm_shared_region_t* region) {
    if (!region) {
        return -1;
    }
    if (region->refcount != 0) {
        return 0;
    }
    pfa_free_pages(region->base, region->pages);
    list_remove(&g_shared_list, region);
    return 0;
}

static mem_region_t* mm_find_region_for_addr(mm_context_t* ctx, uint64_t addr) {
    if (!ctx) {
        return 0;
    }
    list_iter_t it;
    mem_region_t* region = (mem_region_t*)list_first(&ctx->regions, &it);
    while (region) {
        uint64_t end = region->base + region->size;
        if (addr >= region->base && addr < end) {
            return region;
        }
        region = (mem_region_t*)list_next(&it);
    }
    return 0;
}

static int mm_region_flags_valid(uint32_t flags) {
    if ((flags & MEM_REGION_FLAG_USER) && (flags & MEM_REGION_FLAG_WRITE) &&
        (flags & MEM_REGION_FLAG_EXEC)) {
        return 0;
    }
    return 1;
}

static uint64_t mm_page_align_up(uint64_t size) {
    if (size == 0) {
        return 0;
    }
    return (size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static mem_region_t* mm_context_add_region_slot(mm_context_t* ctx, uint64_t base, uint64_t size,
                                                uint32_t flags, mem_region_type_t type) {
    mem_region_t* region = 0;
    if (!ctx || !mm_region_flags_valid(flags)) {
        return 0;
    }
    region = (mem_region_t*)list_alloc(&ctx->regions);
    if (!region) {
        return 0;
    }
    memset(region, 0, sizeof(*region));
    region->base = base;
    region->phys_base = 0;
    region->size = size;
    region->flags = flags;
    region->type = type;
    ctx->region_count++;
    return region;
}

static void mm_context_release_regions(mm_context_t* ctx) {
    list_iter_t it;
    mem_region_t* region = 0;
    if (!ctx) {
        return;
    }
    region = (mem_region_t*)list_first(&ctx->regions, &it);
    while (region) {
        if (region->phys_base != 0 && region->size != 0) {
            if (region->type == MEM_REGION_SHARED) {
                /* Physical pages owned by mm_shared_region_t; decrement the pin
                 * acquired in mm_shared_map, then release the logical reference. */
                uint64_t pages = (region->size + PAGE_SIZE - 1ULL) / PAGE_SIZE;
                pfa_free_pages(region->phys_base, pages);
                (void)mm_shared_release(ctx->id, region->shared_id);
            } else if (!(region->flags & MEM_REGION_FLAG_PHYS_EXTERNAL) &&
                       region->backing_pages != 0) {
                /* Free only the owned backing (backing_pages), which for a
                 * WASM_LINEAR region under the slot model is the original
                 * placeholder — NOT region->size, which tracks the (grown)
                 * guest VA extent whose real backing the linmem slot owns. */
                pfa_free_pages(region->phys_base, region->backing_pages);
            }
        }
        region = (mem_region_t*)list_next(&it);
    }
    list_destroy(&ctx->regions);
    ctx->region_count = 0;
}

static void mm_trace_copy_fail(const char* op, const char* stage, uint32_t context_id,
                               uint64_t user_addr, uint64_t size, uint64_t root_expected,
                               uint64_t root_current, uint64_t chunk_user_addr,
                               uint64_t chunk_size) {
    trace_do(klog_write("[mm-copy] fail op="));
    trace_do(klog_write(op));
    trace_do(klog_write(" stage="));
    trace_do(klog_write(stage));
    trace_do(klog_write(" ctx="));
    trace_do(serial_write_hex64((uint64_t)context_id));
    trace_do(klog_write(" user="));
    trace_do(serial_write_hex64(user_addr));
    trace_do(klog_write(" size="));
    trace_do(serial_write_hex64(size));
    trace_do(klog_write(" root_expected="));
    trace_do(serial_write_hex64(root_expected));
    trace_do(klog_write(" root_current="));
    trace_do(serial_write_hex64(root_current));
    trace_do(klog_write(" chunk_user="));
    trace_do(serial_write_hex64(chunk_user_addr));
    trace_do(klog_write(" chunk_size="));
    trace_do(serial_write_hex64(chunk_size));
    trace_do(klog_write("\n"));
}

static int mm_ensure_user_range_mapped(mm_context_t* ctx, uint64_t user_addr, uint64_t size,
                                       uint32_t needed_flags) {
    if (!ctx || ctx->root_table == 0 || user_addr == 0 || size == 0) {
        return -1;
    }
    uint64_t end = user_addr + size;
    if (end < user_addr) {
        return -1;
    }
    uint64_t cur = user_addr;
    while (cur < end) {
        mem_region_t* region = mm_find_region_for_addr(ctx, cur);
        if (!region) {
            return -1;
        }
        if (!(region->flags & MEM_REGION_FLAG_USER) || !(region->flags & MEM_REGION_FLAG_READ) ||
            ((needed_flags & MEM_REGION_FLAG_WRITE) && !(region->flags & MEM_REGION_FLAG_WRITE))) {
            return -1;
        }
        uint64_t page_base = cur & ~(PAGE_SIZE - 1ULL);
        /* Idempotent: leave any page that is already mapped alone.  For a
         * WASM_LINEAR region under the unified linmem model the live backing is
         * the scattered linmem slot (and any overlay physical frames), not the
         * region's contiguous placeholder phys_base; remapping to the
         * placeholder here would silently detach the interpreter's window from
         * its real pages.  Only genuinely unmapped pages fall back to the
         * placeholder (lazy backing for stack/heap/IPC regions). */
        if (paging_virt_to_phys_in_root(ctx->root_table, page_base) != 0) {
            cur = page_base + PAGE_SIZE;
            continue;
        }
        uint64_t phys_page = region->phys_base + (page_base - region->base);
        if (paging_map_4k_in_root(ctx->root_table, page_base, phys_page, region->flags) < 0) {
            return -1;
        }
        cur = page_base + PAGE_SIZE;
    }
    return 0;
}

/* Permission check for a user virtual range, answering 0 for PERMITTED and -1
 * for refused — inverted relative to a boolean predicate, so read the polarity
 * carefully.
 *
 * user_addr is a virtual address in the target context, never a physical or
 * higher-half address.  The whole range must lie within regions that carry
 * MEM_REGION_FLAG_USER and every bit set in needed_flags; a range spanning
 * several regions is checked region by region, and a gap between them is
 * refused.  A zero context_id (the kernel context), a zero address, a zero size,
 * a context without an address space, and an address+size that wraps are all
 * refused.
 *
 * This is a check against the REGION table, not the page tables: it says the
 * access is allowed, not that the pages are currently mapped.  It maps nothing
 * and takes no lock. */
int mm_user_range_permitted(uint32_t context_id, uint64_t user_addr, uint64_t size,
                            uint32_t needed_flags) {
    if (context_id == 0 || user_addr == 0 || size == 0) {
        return -1;
    }

    mm_context_t* ctx = mm_context_get(context_id);
    if (!ctx || ctx->root_table == 0) {
        return -1;
    }

    uint64_t end = user_addr + size;
    if (end < user_addr) {
        return -1;
    }

    uint64_t cur = user_addr;
    while (cur < end) {
        mem_region_t* region = mm_find_region_for_addr(ctx, cur);
        if (!region) {
            return -1;
        }
        if (!(region->flags & MEM_REGION_FLAG_USER)) {
            return -1;
        }
        if ((needed_flags & MEM_REGION_FLAG_READ) && !(region->flags & MEM_REGION_FLAG_READ)) {
            return -1;
        }
        if ((needed_flags & MEM_REGION_FLAG_WRITE) && !(region->flags & MEM_REGION_FLAG_WRITE)) {
            return -1;
        }
        if ((needed_flags & MEM_REGION_FLAG_EXEC) && !(region->flags & MEM_REGION_FLAG_EXEC)) {
            return -1;
        }

        uint64_t page_base = cur & ~(PAGE_SIZE - 1ULL);
        cur = page_base + PAGE_SIZE;
    }

    return 0;
}

/* Demand-maps the page containing `addr` from the region table, turning a
 * not-present fault inside a declared region into a mapping.
 *
 * addr is the faulting virtual address in context_id's address space and
 * error_code is the x86 page-fault error code pushed by the CPU.  A fault whose
 * PRESENT bit is set is a protection violation, not a missing page, and is
 * refused so the caller reports it; the U/S, W/R and I/D bits are checked
 * against the region's flags, so a ring-3 fault on a supervisor region, a write
 * to a read-only region and an instruction fetch from a non-EXEC region are all
 * refused too.
 *
 * The frame installed is region->phys_base + (page VA - region->base), i.e. the
 * region's contiguous backing.  Nothing is allocated here, so a region without
 * backing maps frame 0 onwards rather than failing.
 *
 * Returns 0 once the page is mapped, writing the page-aligned VA to
 * *out_mapped_base when it is non-NULL, and -1 when the fault is not one this
 * function may resolve.  Runs in the fault handler and takes no lock beyond
 * mm_context_get's. */
int mm_handle_page_fault(uint32_t context_id, uint64_t addr, uint64_t error_code,
                         uint64_t* out_mapped_base) {
    mm_context_t* ctx = mm_context_get(context_id);
    if (!ctx) {
        return -1;
    }

    if (error_code & pf_err_present) {
        return -1;
    }

    mem_region_t* region = mm_find_region_for_addr(ctx, addr);
    if (!region) {
        return -1;
    }
    if ((error_code & pf_err_user) && !(region->flags & MEM_REGION_FLAG_USER)) {
        return -1;
    }

    if ((error_code & pf_err_write) && !(region->flags & MEM_REGION_FLAG_WRITE)) {
        return -1;
    }
    if ((error_code & pf_err_instr) && !(region->flags & MEM_REGION_FLAG_EXEC)) {
        return -1;
    }

    uint64_t page_base = addr & ~(PAGE_SIZE - 1ULL);
    /* A region carries a process-visible virtual base and a separate physical
     * backing base, so the faulting page resolves to phys_base + (VA offset into
     * the region) and is wired into the owning context's private root table. */
    uint64_t phys_page = region->phys_base + (page_base - region->base);
    int rc = paging_map_4k_in_root(ctx->root_table, page_base, phys_page, region->flags);
    if (rc < 0) {
        return -1;
    }

    if (out_mapped_base) {
        *out_mapped_base = page_base;
    }
    return 0;
}

/* Allocates `pages` physically contiguous frames and registers them as a shared
 * region owned by owner_context_id.
 *
 * *out_base receives the PHYSICAL base — no virtual mapping exists yet; that is
 * mm_shared_map's job.  *out_id receives the region id, which is never 0.
 *
 * The region is created with refcount 0.  Since the region is only reclaimed
 * from the refcount-reaching-zero path in mm_shared_release/mm_shared_unmap, a
 * region that is created and never retained or mapped holds its frames for the
 * life of the kernel.
 *
 * Returns 0 on success and -1 for a NULL out pointer, a zero page count, frame
 * exhaustion, a failure to find a free id within MM_MAX_SHARED probes, or a full
 * region list; frames taken on those paths are released again.  Takes
 * g_shared_lock. */
int mm_shared_create(uint32_t owner_context_id, uint64_t pages, uint32_t flags, uint32_t* out_id,
                     uint64_t* out_base) {
    if (!out_id || !out_base || pages == 0) {
        return -1;
    }
    ksync_spinlock_lock(&g_shared_lock);
    mm_shared_init_once_locked();
#if WASMOS_WASM_RUNTIME == 1 /* WARP JIT backend */
    /* Shmem takes frames from the zone below WASMOS_SHMEM_PHYS_LIMIT.  WARP's
     * own allocators (warp_kmalloc, MemUtils::allocPagedMemory, the ring-3 JIT
     * pages) take frames at or above that limit, so a bulk zero-fill reaching
     * through their direct-map alias (phys | KERNEL_HIGHER_HALF_BASE) cannot
     * land on an active shmem page.
     * FIXME: linmem_slot_commit() allocates linear-memory frames with plain
     * pfa_alloc_pages(), i.e. without the above-limit floor, so this separation
     * no longer covers slot-backed linear memory. */
    uint64_t base = pfa_alloc_pages_below(pages, WASMOS_SHMEM_PHYS_LIMIT);
#else
    /* wasm3 has no above-limit allocator zone to stay clear of, so shmem may
     * take any free frame. */
    uint64_t base = pfa_alloc_pages(pages);
#endif
    if (!base) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    uint32_t id = 0;
    for (uint32_t tries = 0; tries <= MM_MAX_SHARED; ++tries) {
        uint32_t candidate = g_shared_next_id++;
        if (candidate == 0) {
            candidate = g_shared_next_id++;
        }
        if (!mm_shared_find_locked(candidate)) {
            id = candidate;
            break;
        }
    }
    if (id == 0) {
        pfa_free_pages(base, pages);
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    mm_shared_region_t* region = (mm_shared_region_t*)list_alloc(&g_shared_list);
    if (!region) {
        pfa_free_pages(base, pages);
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    memset(region, 0, sizeof(*region));
    region->id = id;
    region->owner_context_id = owner_context_id;
    region->refcount = 0;
    region->base = base;
    region->pages = pages;
    region->flags = flags;
    region->grant_count = 0;
    *out_id = id;
    *out_base = base;
    ksync_spinlock_unlock(&g_shared_lock);
    return 0;
}

/* Adds target_context_id to a region's grant list, letting it pass the access
 * check in the other mm_shared_* calls.  Granting does not map anything.
 *
 * owner_context_id authorises the change: 0 is the kernel and may grant on any
 * region, any other value must match the region's owner.  Granting to the owner
 * itself, or re-granting an existing grantee, succeeds without changing
 * anything.
 *
 * Returns 0 on success and -1 for an unknown id, a zero target, a non-owner
 * caller, or a grant list already at MM_MAX_SHARED_GRANTS.  Takes
 * g_shared_lock. */
int mm_shared_grant(uint32_t owner_context_id, uint32_t id, uint32_t target_context_id) {
    ksync_spinlock_lock(&g_shared_lock);
    mm_shared_region_t* region = mm_shared_find_locked(id);
    uint8_t i = 0;
    if (!region || target_context_id == 0) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    if (owner_context_id != 0 && region->owner_context_id != owner_context_id) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    if (region->owner_context_id == target_context_id) {
        ksync_spinlock_unlock(&g_shared_lock);
        return 0;
    }
    for (i = 0; i < region->grant_count && i < MM_MAX_SHARED_GRANTS; ++i) {
        if (region->grant_contexts[i] == target_context_id) {
            ksync_spinlock_unlock(&g_shared_lock);
            return 0;
        }
    }
    if (region->grant_count >= MM_MAX_SHARED_GRANTS) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    region->grant_contexts[region->grant_count++] = target_context_id;
    ksync_spinlock_unlock(&g_shared_lock);
    return 0;
}

/* Removes target_context_id from a region's grant list.  Revoking withdraws
 * future permission only: it does NOT unmap the region from a context that has
 * already mapped it, nor drop the reference that mapping took.
 *
 * Authorised like mm_shared_grant.  Revoking a context that holds no grant, or
 * the owner itself, succeeds and changes nothing, so 0 does not imply a grant
 * was removed.  Returns -1 for an unknown id, a zero target, or a non-owner
 * caller.  Takes g_shared_lock. */
int mm_shared_revoke(uint32_t owner_context_id, uint32_t id, uint32_t target_context_id) {
    ksync_spinlock_lock(&g_shared_lock);
    mm_shared_region_t* region = mm_shared_find_locked(id);
    uint8_t i = 0;
    if (!region || target_context_id == 0) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    if (owner_context_id != 0 && region->owner_context_id != owner_context_id) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    if (region->owner_context_id == target_context_id) {
        ksync_spinlock_unlock(&g_shared_lock);
        return 0;
    }
    for (i = 0; i < region->grant_count && i < MM_MAX_SHARED_GRANTS; ++i) {
        if (region->grant_contexts[i] != target_context_id) {
            continue;
        }
        for (uint8_t j = i; j + 1 < region->grant_count; ++j) {
            region->grant_contexts[j] = region->grant_contexts[j + 1];
        }
        if (region->grant_count > 0) {
            region->grant_count--;
            region->grant_contexts[region->grant_count] = 0;
        }
        ksync_spinlock_unlock(&g_shared_lock);
        return 0;
    }
    ksync_spinlock_unlock(&g_shared_lock);
    return 0;
}

/* Reports a region's PHYSICAL base and page count to any context that owns it,
 * holds a grant, or is the kernel (context 0).  Takes no reference: the caller
 * must already hold one, or the frames can be freed underneath the value it just
 * read.  Returns 0 on success, -1 for a NULL out pointer, an unknown id, or a
 * caller without access.  Takes g_shared_lock. */
int mm_shared_get_phys(uint32_t owner_context_id, uint32_t id, uint64_t* out_base,
                       uint64_t* out_pages) {
    if (!out_base || !out_pages) {
        return -1;
    }
    ksync_spinlock_lock(&g_shared_lock);
    mm_shared_region_t* region = mm_shared_find_locked(id);
    if (!region || !mm_shared_access_allowed(region, owner_context_id)) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    *out_base = region->base;
    *out_pages = region->pages;
    ksync_spinlock_unlock(&g_shared_lock);
    return 0;
}

/* Takes one reference on the region.  The count is a count of LOGICAL holders —
 * every mm_shared_retain and every mm_shared_map adds one, and the region's
 * frames are released when it falls back to 0.  It is not a mapping count and
 * not the per-frame count physmem.c keeps.
 *
 * Returns 0 on success, -1 for an unknown id, a caller without access, or a
 * count already at UINT32_MAX (refused rather than wrapped).  Takes
 * g_shared_lock.  Pair every success with exactly one mm_shared_release. */
int mm_shared_retain(uint32_t owner_context_id, uint32_t id) {
    ksync_spinlock_lock(&g_shared_lock);
    mm_shared_region_t* region = mm_shared_find_locked(id);
    if (!region || !mm_shared_access_allowed(region, owner_context_id)) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    if (region->refcount == UINT32_MAX) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    region->refcount++;
    ksync_spinlock_unlock(&g_shared_lock);
    return 0;
}

/* Drops one reference.  When the count reaches 0 the region's frames go back to
 * the allocator and the region record is removed, so `id` becomes invalid and
 * any mapping still pointing at those frames is left dangling — release only
 * after unmapping.
 *
 * Returns 0 both when a reference was merely dropped and when the region was
 * freed; the two are indistinguishable to the caller.  Returns -1 for an unknown
 * id, a caller without access, or a count already at 0, so a double release is
 * refused rather than wrapping.  Takes g_shared_lock. */
int mm_shared_release(uint32_t owner_context_id, uint32_t id) {
    ksync_spinlock_lock(&g_shared_lock);
    mm_shared_region_t* region = mm_shared_find_locked(id);
    if (!region || !mm_shared_access_allowed(region, owner_context_id) || region->refcount == 0) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    region->refcount--;
    int rc = mm_shared_free_if_unused(region);
    ksync_spinlock_unlock(&g_shared_lock);
    return rc;
}

/* Gives ctx a MEM_REGION_SHARED region over the shared object, at a fresh VA
 * taken from the context's rolling shared window.  *out_base receives that
 * VIRTUAL base; the pages are not wired into the page tables here, so they map
 * on demand through mm_handle_page_fault.
 *
 * flags intersects with the region's creation flags, so it can only narrow
 * access; passing 0 keeps the creation flags unchanged.
 *
 * Two independent counts are taken: one logical reference on the shared region
 * (the same count mm_shared_retain moves), and a physmem pin on each frame so
 * the frames survive even if the logical count drops elsewhere.
 * mm_context_release_regions undoes both at teardown.
 *
 * Returns 0 on success, -1 for a NULL ctx, an unknown id, a caller without
 * access, a saturated reference count, or a failure to place the region — the
 * reference is given back on that last path.  Takes g_shared_lock.
 *
 * The VA window only ever moves forward; repeated map/unmap cycles consume
 * address space rather than reusing it. */
int mm_shared_map(mm_context_t* ctx, uint32_t id, uint32_t flags, uint64_t* out_base) {
    if (!ctx) {
        return -1;
    }
    ksync_spinlock_lock(&g_shared_lock);
    mm_shared_region_t* region = mm_shared_find_locked(id);
    if (!region || !mm_shared_access_allowed(region, ctx->id)) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    uint32_t effective_flags = region->flags;
    if (flags) {
        effective_flags &= flags;
    }
    if (region->refcount == UINT32_MAX) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    region->refcount++;
    uint64_t virt_base = mm_region_virtual_base(ctx, MEM_REGION_SHARED, region->pages);
    mem_region_t* added = 0;
    if (virt_base != 0) {
        added = mm_context_add_region_slot(ctx, virt_base, region->pages * PAGE_SIZE,
                                           effective_flags, MEM_REGION_SHARED);
    }
    if (!added) {
        region->refcount--;
        (void)mm_shared_free_if_unused(region);
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    added->phys_base = region->base;
    added->shared_id = id;
    pfa_pin_pages(region->base, region->pages);
    if (out_base) {
        *out_base = virt_base;
    }
    ksync_spinlock_unlock(&g_shared_lock);
    return 0;
}

/* Removes ctx's region entry for the shared object and drops the logical
 * reference mm_shared_map took.  The matching region is found by physical base
 * and size, so a context that mapped the same object twice loses whichever entry
 * the list yields first.
 *
 * It does NOT unmap the pages from the page tables and does NOT undo the
 * physmem pin mm_shared_map installed, so the frames stay pinned and any PTE
 * already faulted in stays live.
 *
 * Returns 0 on success, -1 for a NULL ctx, an unknown id, a caller without
 * access, no matching region in this context, or a reference count already at 0.
 * Takes g_shared_lock. */
int mm_shared_unmap(mm_context_t* ctx, uint32_t id) {
    if (!ctx) {
        return -1;
    }
    ksync_spinlock_lock(&g_shared_lock);
    mm_shared_region_t* region = mm_shared_find_locked(id);
    if (!region || !mm_shared_access_allowed(region, ctx->id)) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }

    uint32_t found = 0;
    list_iter_t it;
    mem_region_t* r = (mem_region_t*)list_first(&ctx->regions, &it);
    while (r) {
        if (r->type == MEM_REGION_SHARED && r->phys_base == region->base &&
            r->size == region->pages * PAGE_SIZE) {
            if (list_remove(&ctx->regions, r) == 0) {
                if (ctx->region_count > 0) {
                    ctx->region_count--;
                }
                found = 1;
            }
            break;
        }
        r = (mem_region_t*)list_next(&it);
    }
    if (!found) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    if (region->refcount == 0) {
        ksync_spinlock_unlock(&g_shared_lock);
        return -1;
    }
    region->refcount--;
    int rc = mm_shared_free_if_unused(region);
    ksync_spinlock_unlock(&g_shared_lock);
    return rc;
}

/* Allocates `pages` physically contiguous frames and records a region of the
 * given type over them at the type's fixed VA (MEM_REGION_SHARED instead takes
 * the next slice of the rolling shared window).  Nothing is mapped; the pages
 * arrive through mm_handle_page_fault on first touch.
 *
 * The region OWNS these frames: backing_pages is set to `pages`, which is what
 * mm_context_release_regions later frees — exactly once, and independently of
 * region->size, which can grow past the backing under the linmem slot model.
 *
 * MEM_REGION_CODE has no fixed VA and is therefore always refused.  One region
 * per type is the implicit model; calling this twice for the same type creates
 * two regions at the same base.
 *
 * Returns 0 on success, -1 for a NULL ctx, a zero page count, frame exhaustion,
 * a type with no VA, or a full region list; the frames are released again on the
 * failure paths. */
int mm_context_alloc_region(mm_context_t* ctx, uint64_t pages, uint32_t flags,
                            mem_region_type_t type) {
    if (!ctx || pages == 0) {
        return -1;
    }
    uint64_t phys = pfa_alloc_pages(pages);
    if (!phys) {
        return -1;
    }
    uint64_t virt = mm_region_virtual_base(ctx, type, pages);
    if (!virt) {
        pfa_free_pages(phys, pages);
        return -1;
    }
    mem_region_t* added = mm_context_add_region_slot(ctx, virt, pages * PAGE_SIZE, flags, type);
    if (!added) {
        pfa_free_pages(phys, pages);
        return -1;
    }
    added->phys_base = phys;
    added->backing_pages = pages;
    return 0;
}

/* Repoints a context's WASM_LINEAR region at a different, physically CONTIGUOUS
 * backing — the runtime's own linear-memory allocation — leaving the region's
 * user VA unchanged.  mm_context_bind_wasm_linear_scattered is the counterpart
 * for backing that is not contiguous.
 *
 * phys_base is a page-aligned PHYSICAL address that the CALLER owns: the region
 * is marked MEM_REGION_FLAG_PHYS_EXTERNAL, so mm_context_release_regions will
 * not free it, and freeing it while the region still refers to it leaves the
 * guest mapped over reusable frames.
 *
 * Any previously owned (non-external) backing at a different address IS freed
 * here, so the previous owner must not free it again.  Every page of the old and
 * new extents is unmapped first, which drops mappings the guest had already
 * faulted in; they are re-established on the next fault.
 *
 * Rebinding to exactly the current external backing is a no-op returning 0.
 * Returns -1 for a zero context, a zero or misaligned phys_base, a zero size, an
 * unknown context, or a context with no WASM_LINEAR region. */
int mm_context_rebind_wasm_linear(uint32_t context_id, uint64_t phys_base, uint64_t size) {
    mm_context_t* ctx = 0;
    mem_region_t* region = 0;
    list_iter_t it;
    uint64_t old_phys = 0;
    uint64_t old_size = 0;
    uint32_t old_flags = 0;
    uint64_t unmap_bytes = 0;

    if (context_id == 0 || phys_base == 0 || size == 0 || (phys_base & (PAGE_SIZE - 1ULL)) != 0) {
        return -1;
    }

    ctx = mm_context_get(context_id);
    if (!ctx) {
        return -1;
    }

    region = (mem_region_t*)list_first(&ctx->regions, &it);
    while (region) {
        if (region->type == MEM_REGION_WASM_LINEAR) {
            break;
        }
        region = (mem_region_t*)list_next(&it);
    }
    if (!region) {
        return -1;
    }

    old_phys = region->phys_base;
    old_size = region->size;
    old_flags = region->flags;
    if (old_phys == phys_base && old_size == size && (old_flags & MEM_REGION_FLAG_PHYS_EXTERNAL)) {
        return 0;
    }

    unmap_bytes = mm_page_align_up(old_size);
    if (mm_page_align_up(size) > unmap_bytes) {
        unmap_bytes = mm_page_align_up(size);
    }
    for (uint64_t off = 0; off < unmap_bytes; off += PAGE_SIZE) {
        (void)paging_unmap_4k_in_root(ctx->root_table, region->base + off);
    }

    if (old_phys != 0 && !(old_flags & MEM_REGION_FLAG_PHYS_EXTERNAL) && old_phys != phys_base) {
        uint64_t old_pages = (old_size + PAGE_SIZE - 1ULL) / PAGE_SIZE;
        if (old_pages != 0) {
            pfa_free_pages(old_phys, old_pages);
        }
    }

    region->phys_base = phys_base;
    region->size = size;
    region->flags |= MEM_REGION_FLAG_PHYS_EXTERNAL;
    return 0;
}

/* Bind the WASM_LINEAR user-region VA to a SCATTERED kernel backing (a
 * reserved-VA linmem slot, whose pages are not physically contiguous).  Maps
 * region page P of the range [from_page, to_page) to the frame backing
 * slot_va_base + P*PAGE_SIZE, so the user-region alias tracks the slot-VA view
 * page for page.  The single-phys_base mm_context_rebind_wasm_linear cannot
 * describe scattered backing, which is why both exist.
 * Returns 0 on success, -1 on a bad argument, a missing WASM_LINEAR region, an
 * unmapped slot page, or a failed mapping (internal status, not a packed code). */
int mm_context_bind_wasm_linear_scattered(uint32_t context_id, uint64_t slot_va_base,
                                          uint64_t from_page, uint64_t to_page) {
    mm_context_t* ctx = 0;
    mem_region_t* region = 0;
    list_iter_t it;
    uint64_t new_size = 0;

    if (context_id == 0 || slot_va_base == 0 || to_page <= from_page ||
        (slot_va_base & (PAGE_SIZE - 1ULL)) != 0) {
        return -1;
    }
    ctx = mm_context_get(context_id);
    if (!ctx) {
        return -1;
    }
    region = (mem_region_t*)list_first(&ctx->regions, &it);
    while (region) {
        if (region->type == MEM_REGION_WASM_LINEAR) {
            break;
        }
        region = (mem_region_t*)list_next(&it);
    }
    if (!region) {
        return -1;
    }

    new_size = to_page * PAGE_SIZE;

    /* Point the WASM_LINEAR user-VA window at the linmem slot's scattered frames,
     * page for page (region page P -> slot frame P), for the [from_page, to_page)
     * tail only.  Binding just the freshly committed tail keeps a grow from
     * clobbering overlays (shmem / framebuffer / DMA / net ring) that earlier
     * hostcalls mapped over lower pages.  The slot OWNS these frames and frees
     * them via its own decommit at reap; this region must never free them.
     *
     * The region's original placeholder is deliberately NOT freed here, and the
     * region is NOT marked PHYS_EXTERNAL.  The placeholder (phys_base /
     * backing_pages) stays owned by the region and is freed exactly once by
     * mm_context_release_regions at teardown.  Freeing it here mid-life returns
     * its pages to the allocator, where a later linmem slot commit reuses them,
     * and the slot's decommit then double-frees them. */
    for (uint64_t p = from_page; p < to_page; ++p) {
        uint64_t off = p * PAGE_SIZE;
        uint64_t phys = paging_virt_to_phys(slot_va_base + off) & ~(PAGE_SIZE - 1ULL);
        if (phys == 0) {
            return -1;
        }
        (void)paging_unmap_4k_in_root(ctx->root_table, region->base + off);
        if (paging_map_4k_in_root(ctx->root_table, region->base + off, phys,
                                  MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                      MEM_REGION_FLAG_USER) != 0) {
            return -1;
        }
    }

    /* Grow the VA extent for bounds checks; backing_pages stays the placeholder
     * count so release frees only that, never the slot-owned live pages. */
    if (new_size > region->size) {
        region->size = new_size;
    }
    return 0;
}

/* The fixed user VA at which every context's WASM_LINEAR region starts.  A
 * compile-time constant shared by all contexts, so a guest linear-memory offset
 * converts to a user VA by adding it — but only in that context's address
 * space. */
uint64_t mm_user_wasm_linear_base(void) {
    return MM_USER_LINEAR_BASE;
}

static mm_context_t* mm_context_get_locked(uint32_t id) {
    if (id == g_root_ctx.id) {
        return &g_root_ctx;
    }
    list_iter_t it;
    mm_context_t* ctx = (mm_context_t*)list_first(&g_contexts, &it);
    while (ctx) {
        if (ctx->id == id) {
            return ctx;
        }
        ctx = (mm_context_t*)list_next(&it);
    }
    return 0;
}

/* Looks up a context by id, returning a pointer to the LIVE record rather than a
 * copy, so writes through it change the context.  Id 0 resolves to the static
 * root context.  Returns 0 when no context has that id.
 *
 * The lock is released before returning, so the pointer is only as stable as the
 * caller's own guarantee that the context is not concurrently destroyed;
 * mm_context_destroy invalidates it.  The list grows by whole chunks and does
 * not relocate existing entries, so an unrelated context being created does not
 * move it. */
mm_context_t* mm_context_get(uint32_t id) {
    ksync_spinlock_lock(&g_contexts_lock);
    mm_context_t* ctx = mm_context_get_locked(id);
    ksync_spinlock_unlock(&g_contexts_lock);
    return ctx;
}

/* Creates a context with its own address space and the standard user regions —
 * 64 KiB of WASM_LINEAR, 8 KiB of stack, 16 KiB of heap, all USER|READ|WRITE —
 * and verifies the resulting root against the shared-kernel layout before
 * publishing it.
 *
 * Returns the live context, or 0 when the id is already taken, the list is full,
 * an address space or region cannot be created, or the root fails verification.
 * Every failure after the first step unwinds fully: regions released, address
 * space destroyed, list entry removed.
 *
 * Id 0 is special-cased to the static root context and is returned as an
 * existing context rather than refused as a duplicate.  Holds g_contexts_lock
 * for the whole construction, so it must not be called from anything already
 * holding it. */
mm_context_t* mm_context_create(uint32_t id) {
    ksync_spinlock_lock(&g_contexts_lock);
    if (id == g_root_ctx.id) {
        ksync_spinlock_unlock(&g_contexts_lock);
        return &g_root_ctx;
    }
    if (mm_context_get_locked(id)) {
        ksync_spinlock_unlock(&g_contexts_lock);
        return 0;
    }
    mm_context_t* ctx = (mm_context_t*)list_alloc(&g_contexts);
    if (!ctx) {
        ksync_spinlock_unlock(&g_contexts_lock);
        return 0;
    }
    if (mm_context_init(ctx, id) != 0) {
        (void)list_remove(&g_contexts, ctx);
        ksync_spinlock_unlock(&g_contexts_lock);
        return 0;
    }
    if (paging_create_address_space(&ctx->root_table) != 0) {
        list_destroy(&ctx->regions);
        (void)list_remove(&g_contexts, ctx);
        ksync_spinlock_unlock(&g_contexts_lock);
        return 0;
    }
    /* wasm3 instantiates modules with one wasm page (64 KiB) of initial linear
     * memory.  The user-VA mirror region must cover that whole range, otherwise
     * host calls that reconcile the user view (wasm_copy_*_user_sync_views)
     * reject pointers whose offset lands above the region — e.g. a service whose
     * stack sits high in linear memory failing svc_register.  16 * 4 KiB frames
     * == 64 KiB, matching the wasm3 initial memory (and the root context). */
    if (mm_context_alloc_region(ctx, 16,
                                MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER,
                                MEM_REGION_WASM_LINEAR) != 0) {
        mm_context_release_regions(ctx);
        paging_destroy_address_space(ctx->root_table);
        (void)list_remove(&g_contexts, ctx);
        ksync_spinlock_unlock(&g_contexts_lock);
        return 0;
    }
    if (mm_context_alloc_region(ctx, 2,
                                MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER,
                                MEM_REGION_STACK) != 0) {
        mm_context_release_regions(ctx);
        paging_destroy_address_space(ctx->root_table);
        (void)list_remove(&g_contexts, ctx);
        ksync_spinlock_unlock(&g_contexts_lock);
        return 0;
    }
    if (mm_context_alloc_region(ctx, 4,
                                MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER,
                                MEM_REGION_HEAP) != 0) {
        mm_context_release_regions(ctx);
        paging_destroy_address_space(ctx->root_table);
        (void)list_remove(&g_contexts, ctx);
        ksync_spinlock_unlock(&g_contexts_lock);
        return 0;
    }
    if (paging_verify_user_root(ctx->root_table, 1) != 0) {
        klog_write("[mm] verify user root failed\n");
        paging_dump_user_root_kernel_mappings(ctx->root_table);
        mm_context_release_regions(ctx);
        paging_destroy_address_space(ctx->root_table);
        (void)list_remove(&g_contexts, ctx);
        ksync_spinlock_unlock(&g_contexts_lock);
        return 0;
    }
    ksync_spinlock_unlock(&g_contexts_lock);
    return ctx;
}

/* Tears a context down: frees the frames its regions own, drops its shared-region
 * references, frees the page-table structure, and removes the list entry.  Any
 * mm_context_t* previously handed out for this id becomes dangling.
 *
 * The root context (id 0) is refused, since the kernel's own address space must
 * not be destroyed.  Returns 0 on success, -1 for id 0 and for an unknown id.
 *
 * It does not check whether a CPU is currently running on this root; the caller
 * must have switched away first.  Holds g_contexts_lock throughout. */
int mm_context_destroy(uint32_t id) {
    ksync_spinlock_lock(&g_contexts_lock);
    if (id == g_root_ctx.id || id == 0) {
        ksync_spinlock_unlock(&g_contexts_lock);
        return -1;
    }
    mm_context_t* ctx = mm_context_get_locked(id);
    if (!ctx) {
        ksync_spinlock_unlock(&g_contexts_lock);
        return -1;
    }
    mm_context_release_regions(ctx);
    paging_destroy_address_space(ctx->root_table);
    (void)list_remove(&g_contexts, ctx);
    ksync_spinlock_unlock(&g_contexts_lock);
    return 0;
}

/* Loads the context's root into the executing CPU's CR3, which flushes that
 * CPU's non-global TLB entries.  Returns 0 on success and -1 for an unknown
 * context or one without an address space (paging_switch_root itself only fails
 * on a zero root).  Affects this CPU only. */
int mm_context_activate(uint32_t id) {
    mm_context_t* ctx = mm_context_get(id);
    if (!ctx || ctx->root_table == 0) {
        return -1;
    }
    /* Scheduling flips CR3 between the kernel root context and the selected
     * process context. The kernel mappings stay shared; the private user slot
     * changes with the owning mm_context. */
    return paging_switch_root(ctx->root_table);
}

/* The context's root as a PHYSICAL frame address, suitable for CR3 or for the
 * paging_*_in_root family.  Returns 0 for an unknown context and for one whose
 * address space has not been created — the two are indistinguishable. */
uint64_t mm_context_root_table(uint32_t id) {
    mm_context_t* ctx = mm_context_get(id);
    if (!ctx) {
        return 0;
    }
    return ctx->root_table;
}

typedef struct {
    uint32_t context_id;
    uint64_t user_addr;
    uint64_t size;
    uint64_t root_table;
    uint64_t prev_root;
    uint8_t* kernel_ptr;
} mm_copy_from_user_args_t;

typedef struct {
    uint32_t context_id;
    uint64_t user_addr;
    uint64_t size;
    uint64_t root_table;
    uint64_t prev_root;
    const uint8_t* kernel_ptr;
} mm_copy_to_user_args_t;

/* Read from a user address space; runs under mm_run_on_copy_stack.
 *
 * The copy must be chunked through a stack-allocated bounce buffer because
 * `dst` is a kernel pointer that is NOT mapped in the user page table.
 * Per iteration:
 *   1. Switch CR3 to the user page table (user_cur becomes accessible).
 *   2. memcpy into bounce[] — a local stack buffer that IS in both maps.
 *   3. Switch CR3 back to the previous kernel root immediately.
 *   4. memcpy from bounce[] into the kernel dst — now under kernel tables.
 *
 * Chunks are 256 bytes to keep the bounce buffer small (stack space is
 * limited on the copy-stack) while amortising the two CR3 switches per
 * iteration over a reasonable number of bytes. */
static int mm_copy_from_user_impl(void* opaque) {
    mm_copy_from_user_args_t* args = (mm_copy_from_user_args_t*)opaque;
    if (!args) {
        return -1;
    }
    uint8_t* dst_bytes = args->kernel_ptr;
    uint64_t remaining = args->size;
    uint64_t user_cur = args->user_addr;
    const uint64_t chunk_size = 256ULL;
    uint8_t bounce[256];

    while (remaining > 0) {
        uint64_t n = (remaining < chunk_size) ? remaining : chunk_size;
        if (paging_switch_root(args->root_table) != 0) {
            mm_trace_copy_fail("from", "switch_to_user", args->context_id, args->user_addr,
                               args->size, args->root_table, paging_get_current_root_table(),
                               user_cur, n);
            return -1;
        }
        memcpy(bounce, ptr_cast(void, user_cur), (size_t)n);
        if (paging_switch_root(args->prev_root) != 0) {
            mm_trace_copy_fail("from", "switch_to_prev", args->context_id, args->user_addr,
                               args->size, args->prev_root, paging_get_current_root_table(),
                               user_cur, n);
            /* Cannot return: the CPU is still under the user page table, so
             * every kernel address is unmapped and unwinding would fault.
             * kpanic emits the reason before halting; a bare halt would wedge
             * the machine with nothing on the wire. */
            kpanic("mm: cannot restore the kernel page table", args->context_id,
                   (uint64_t)args->user_addr);
        }
        memcpy(dst_bytes, bounce, (size_t)n);
        dst_bytes += n;
        user_cur += n;
        remaining -= n;
    }
    return 0;
}

/* Copies `size` bytes out of another address space: user_src is a VIRTUAL
 * address in context_id's address space and dst is an ordinary kernel pointer,
 * borrowed for the call.  The two are never valid at the same time, which is why
 * the copy is chunked through a bounce buffer with a CR3 flip per chunk.
 *
 * The range is permission-checked and demand-mapped first, so it must lie in
 * USER regions carrying READ.  The copy then runs on this CPU's higher-half
 * copy stack, because the process root has no low identity mapping.
 *
 * Returns 0 on success and -1 for a zero context, a NULL dst, a zero user_src,
 * a zero size, an unknown context, a context without an address space, or a
 * range that is not permitted or cannot be mapped.  A partial copy is possible
 * on the -1 from the copy itself.  Panics if the kernel root cannot be restored
 * mid-copy, since returning would fault on every kernel address.
 *
 * Not reentrant on one CPU: the copy stack is per-CPU and switching CR3 mid-copy
 * means this must not be called from an interrupt that can land inside it. */
int mm_copy_from_user(uint32_t context_id, void* dst, uint64_t user_src, uint64_t size) {
    if (context_id == 0 || !dst || user_src == 0 || size == 0) {
        mm_trace_copy_fail("from", "arg", context_id, user_src, size, 0,
                           paging_get_current_root_table(), user_src, 0);
        return -1;
    }
    mm_context_t* ctx = mm_context_get(context_id);
    if (!ctx || ctx->root_table == 0) {
        mm_trace_copy_fail("from", "ctx", context_id, user_src, size, 0,
                           paging_get_current_root_table(), user_src, 0);
        return -1;
    }
    if (mm_ensure_user_range_mapped(ctx, user_src, size, MEM_REGION_FLAG_READ) != 0) {
        mm_trace_copy_fail("from", "map", context_id, user_src, size, ctx->root_table,
                           paging_get_current_root_table(), user_src, size);
        return -1;
    }

    /* Read the ACTUAL current CPU's CR3 rather than the global
     * g_current_pml4_phys, which is last-writer-wins under SMP and can
     * contain another CPU's page table.  Restoring the wrong root after the
     * user-space copy would leave this CPU with a stripped identity map. */
    uint64_t cur_cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    mm_copy_from_user_args_t args = {
        .context_id = context_id,
        .user_addr = user_src,
        .size = size,
        .root_table = ctx->root_table,
        .prev_root = cur_cr3,
        .kernel_ptr = (uint8_t*)dst,
    };
    return mm_run_on_copy_stack(mm_copy_from_user_impl, &args);
}

static int mm_copy_to_user_impl(void* opaque) {
    mm_copy_to_user_args_t* args = (mm_copy_to_user_args_t*)opaque;
    if (!args) {
        return -1;
    }
    const uint8_t* src_bytes = args->kernel_ptr;
    uint64_t remaining = args->size;
    uint64_t user_cur = args->user_addr;
    const uint64_t chunk_size = 256ULL;
    uint8_t bounce[256];

    while (remaining > 0) {
        uint64_t n = (remaining < chunk_size) ? remaining : chunk_size;
        memcpy(bounce, src_bytes, (size_t)n);
        if (paging_switch_root(args->root_table) != 0) {
            mm_trace_copy_fail("to", "switch_to_user", args->context_id, args->user_addr,
                               args->size, args->root_table, paging_get_current_root_table(),
                               user_cur, n);
            return -1;
        }
        memcpy(ptr_cast(void, user_cur), bounce, (size_t)n);
        if (paging_switch_root(args->prev_root) != 0) {
            mm_trace_copy_fail("to", "switch_to_prev", args->context_id, args->user_addr,
                               args->size, args->prev_root, paging_get_current_root_table(),
                               user_cur, n);
            /* Cannot return: the CPU is still under the user page table, so
             * every kernel address is unmapped and unwinding would fault.
             * kpanic emits the reason before halting; a bare halt would wedge
             * the machine with nothing on the wire. */
            kpanic("mm: cannot restore the kernel page table", args->context_id,
                   (uint64_t)args->user_addr);
        }
        src_bytes += n;
        user_cur += n;
        remaining -= n;
    }
    return 0;
}

/* The write direction of mm_copy_from_user: src is a borrowed kernel pointer and
 * user_dst a VIRTUAL address in context_id's address space.  The range must lie
 * in USER regions carrying WRITE.  Same bounce-buffer chunking, same copy-stack
 * requirement, same panic if the kernel root cannot be restored, and the same
 * possibility of a partial write when the copy itself fails.
 *
 * Returns 0 on success, -1 for a zero context, a zero user_dst, a NULL src, a
 * zero size, an unknown context, a context without an address space, or a range
 * that is not permitted or cannot be mapped. */
int mm_copy_to_user(uint32_t context_id, uint64_t user_dst, const void* src, uint64_t size) {
    if (context_id == 0 || user_dst == 0 || !src || size == 0) {
        mm_trace_copy_fail("to", "arg", context_id, user_dst, size, 0,
                           paging_get_current_root_table(), user_dst, 0);
        return -1;
    }
    mm_context_t* ctx = mm_context_get(context_id);
    if (!ctx || ctx->root_table == 0) {
        mm_trace_copy_fail("to", "ctx", context_id, user_dst, size, 0,
                           paging_get_current_root_table(), user_dst, 0);
        return -1;
    }
    if (mm_ensure_user_range_mapped(ctx, user_dst, size, MEM_REGION_FLAG_WRITE) != 0) {
        mm_trace_copy_fail("to", "map", context_id, user_dst, size, ctx->root_table,
                           paging_get_current_root_table(), user_dst, size);
        return -1;
    }

    /* Read the ACTUAL current CPU's CR3 — see mm_copy_from_user for rationale. */
    uint64_t cur_cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    mm_copy_to_user_args_t args = {
        .context_id = context_id,
        .user_addr = user_dst,
        .size = size,
        .root_table = ctx->root_table,
        .prev_root = cur_cr3,
        .kernel_ptr = (const uint8_t*)src,
    };
    return mm_run_on_copy_stack(mm_copy_to_user_impl, &args);
}

/* TODO: Migrate pointer-bearing syscall and IPC entry paths to this helper
 * family so kernel code stops dereferencing user virtual addresses directly. */

/* Overlays an arbitrary PHYSICAL range onto a window of a context's WASM_LINEAR
 * region — the mechanism behind zero-copy device and shared-buffer views into
 * guest linear memory.
 *
 * virt is a user virtual address, phys a physical base, and all three of virt,
 * phys and size must be 4 KiB aligned.  [virt, virt+size) must lie entirely
 * inside the context's WASM_LINEAR region; nothing outside that region can be
 * targeted.  W+X user flags are refused.
 *
 * Each page is unmapped and then mapped to the new frame, so the guest's
 * previous view of that page is replaced.  Neither the old nor the new frames
 * are allocated or freed here: the caller owns `phys`, and the region's own
 * placeholder backing stays owned by the region.  Removing the overlay means
 * remapping, not unmapping, since the region's page-fault path would otherwise
 * restore the placeholder frame.
 *
 * Returns 0 once every page is mapped and -1 on a bad argument, an unknown
 * context, a missing or too-small WASM_LINEAR region, or a mapping failure —
 * after which the overlay is applied only up to the failing page. */
int mm_context_map_physical(uint32_t context_id, uint64_t virt, uint64_t phys, uint64_t size,
                            uint32_t flags) {
    if (context_id == 0 || virt == 0 || phys == 0 || size == 0) {
        return -1;
    }
    if ((virt & 0xFFFULL) != 0 || (phys & 0xFFFULL) != 0 || (size & 0xFFFULL) != 0) {
        return -1;
    }
    if (!mm_region_flags_valid(flags)) {
        return -1;
    }

    mm_context_t* ctx = mm_context_get(context_id);
    if (!ctx || ctx->root_table == 0) {
        return -1;
    }

    mem_region_t linear = {0};
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0) {
        return -1;
    }

    uint64_t region_end = linear.base + linear.size;
    if (virt < linear.base || virt + size > region_end) {
        return -1;
    }

    uint64_t pages = size / PAGE_SIZE;
    if (pages == 0) {
        return -1;
    }

    uint64_t current_virt = virt;
    uint64_t current_phys = phys;
    for (uint64_t i = 0; i < pages; ++i) {
        (void)paging_unmap_4k_in_root(ctx->root_table, current_virt);
        int map_rc = paging_map_4k_in_root(ctx->root_table, current_virt, current_phys, flags);
        if (map_rc != 0) {
            return -1;
        }
        current_virt += PAGE_SIZE;
        current_phys += PAGE_SIZE;
    }
    return 0;
}
