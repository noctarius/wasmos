#include "memory.h"
#include "paging.h"
#include "physmem.h"
#include "serial.h"
#include "string.h"

#define PAGE_SIZE 0x1000ULL
#define MM_USER_LINEAR_BASE 0x0000008000000000ULL
#define MM_USER_STACK_BASE  0x0000008100000000ULL
#define MM_USER_HEAP_BASE   0x0000008200000000ULL
#define MM_USER_IPC_BASE    0x0000008300000000ULL
#define MM_USER_DEVICE_BASE 0x0000008400000000ULL
#define MM_USER_SHARED_BASE 0x0000008500000000ULL
#define MM_COPY_STACK_BYTES 8192u
static const uint64_t pf_err_present = 1ULL << 0;
static const uint64_t pf_err_write = 1ULL << 1;
static const uint64_t pf_err_user = 1ULL << 2;
static const uint64_t pf_err_instr = 1ULL << 4;
static const boot_info_t *g_boot_info;
static mm_context_t g_contexts[MM_MAX_CONTEXTS];
static uint32_t g_context_count;
static mm_context_t g_root_ctx;
static uint8_t g_mm_copy_stack[MM_COPY_STACK_BYTES] __attribute__((aligned(16)));

static inline uintptr_t
mm_kernel_alias_addr(uintptr_t addr)
{
    if ((uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        return (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return addr;
}

static inline mm_context_t *
mm_context_table(void)
{
    return (mm_context_t *)(void *)mm_kernel_alias_addr((uintptr_t)&g_contexts[0]);
}

static inline uint32_t *
mm_context_count_ptr(void)
{
    return (uint32_t *)(void *)mm_kernel_alias_addr((uintptr_t)&g_context_count);
}

#define MM_MAX_SHARED 16
#define MM_MAX_SHARED_GRANTS 8
typedef struct {
    uint32_t id;
    uint32_t owner_context_id;
    uint8_t in_use;
    uint32_t refcount;
    uint64_t base;
    uint64_t pages;
    uint32_t flags;
    uint32_t grant_contexts[MM_MAX_SHARED_GRANTS];
    uint8_t grant_count;
} mm_shared_region_t;

static mm_shared_region_t g_shared[MM_MAX_SHARED];
static uint32_t g_shared_next_id = 1;
static int mm_region_flags_valid(uint32_t flags);
typedef int (*mm_copy_work_fn)(void *arg);

static int
mm_run_on_copy_stack(mm_copy_work_fn fn, void *arg)
{
    if (!fn) {
        return -1;
    }

    uint64_t rsp = 0;
    uint64_t higher_half_base = paging_get_higher_half_base();
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    if (rsp >= higher_half_base) {
        return fn(arg);
    }

    uintptr_t stack_top = mm_kernel_alias_addr((uintptr_t)&g_mm_copy_stack[MM_COPY_STACK_BYTES]);
    stack_top &= ~(uintptr_t)0xFULL;
    int rc = -1;
    __asm__ volatile(
        "mov %%rsp, %%r15\n"
        "mov %[stack_top], %%rsp\n"
        "mov %[arg], %%rdi\n"
        "call *%[fn]\n"
        "mov %%r15, %%rsp\n"
        : "=a"(rc)
        : [stack_top] "r"(stack_top), [fn] "r"(fn), [arg] "r"(arg)
        : "r15", "rdi", "rcx", "rdx", "rsi", "r8", "r9", "r10", "memory", "cc");
    return rc;
}

static uint64_t
mm_region_virtual_base(mm_context_t *ctx, mem_region_type_t type, uint64_t pages)
{
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

void mm_init(const boot_info_t *boot_info) {
    g_boot_info = boot_info;
    g_context_count = 0;
    serial_write("[mm] init\n");
    pfa_init(boot_info);
    if (paging_init() != 0) {
        serial_write("[mm] paging init failed\n");
    } else {
        serial_write("[mm] paging init\n");
    }

    if (mm_context_init(&g_root_ctx, 0) == 0) {
        g_root_ctx.root_table = paging_get_root_table();
        mm_context_alloc_region(&g_root_ctx, 16, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_WASM_LINEAR);
        mm_context_alloc_region(&g_root_ctx, 4, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_STACK);
        mm_context_alloc_region(&g_root_ctx, 8, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_HEAP);
        mm_context_alloc_region(&g_root_ctx, 2, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_IPC);
        mm_context_alloc_region(&g_root_ctx, 2, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, MEM_REGION_DEVICE);
    }

    serial_write("[mm] ctx0 regions=");
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    uint64_t value = g_root_ctx.region_count;
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);

}

int mm_context_init(mm_context_t *ctx, uint32_t id) {
    if (!ctx) {
        return -1;
    }
    ctx->id = id;
    ctx->root_table = 0;
    ctx->next_shared_base = MM_USER_SHARED_BASE;
    ctx->region_count = 0;
    for (uint32_t i = 0; i < MM_MAX_REGIONS; ++i) {
        ctx->regions[i].base = 0;
        ctx->regions[i].phys_base = 0;
        ctx->regions[i].size = 0;
        ctx->regions[i].flags = 0;
        ctx->regions[i].type = MEM_REGION_WASM_LINEAR;
    }
    return 0;
}

int mm_context_add_region(mm_context_t *ctx, uint64_t base, uint64_t size, uint32_t flags, mem_region_type_t type) {
    if (!ctx || ctx->region_count >= MM_MAX_REGIONS) {
        return -1;
    }
    if (!mm_region_flags_valid(flags)) {
        return -1;
    }
    mem_region_t *region = &ctx->regions[ctx->region_count++];
    region->base = base;
    region->phys_base = 0;
    region->size = size;
    region->flags = flags;
    region->type = type;
    return 0;
}

int mm_context_region_for_type(mm_context_t *ctx, mem_region_type_t type, mem_region_t *out_region) {
    if (!ctx || !out_region) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        if (ctx->regions[i].type == type) {
            *out_region = ctx->regions[i];
            return 0;
        }
    }
    return -1;
}

static mm_shared_region_t *mm_shared_find(uint32_t id) {
    if (id == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < MM_MAX_SHARED; ++i) {
        if (g_shared[i].in_use && g_shared[i].id == id) {
            return &g_shared[i];
        }
    }
    return 0;
}

static int
mm_shared_access_allowed(const mm_shared_region_t *region, uint32_t context_id)
{
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

static int mm_shared_free_if_unused(mm_shared_region_t *region) {
    if (!region) {
        return -1;
    }
    if (region->refcount != 0) {
        return 0;
    }
    pfa_free_pages(region->base, region->pages);
    region->in_use = 0;
    region->id = 0;
    region->base = 0;
    region->pages = 0;
    region->flags = 0;
    return 0;
}

static mem_region_t *mm_find_region_for_addr(mm_context_t *ctx, uint64_t addr) {
    if (!ctx) {
        return 0;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *region = &ctx->regions[i];
        uint64_t end = region->base + region->size;
        if (addr >= region->base && addr < end) {
            return region;
        }
    }
    return 0;
}

static int
mm_region_flags_valid(uint32_t flags)
{
    if ((flags & MEM_REGION_FLAG_USER) &&
        (flags & MEM_REGION_FLAG_WRITE) &&
        (flags & MEM_REGION_FLAG_EXEC)) {
        return 0;
    }
    return 1;
}

static void
mm_trace_copy_fail(const char *op,
                   const char *stage,
                   uint32_t context_id,
                   uint64_t user_addr,
                   uint64_t size,
                   uint64_t root_expected,
                   uint64_t root_current,
                   uint64_t chunk_user_addr,
                   uint64_t chunk_size)
{
    trace_do(serial_write("[mm-copy] fail op="));
    trace_do(serial_write(op));
    trace_do(serial_write(" stage="));
    trace_do(serial_write(stage));
    trace_do(serial_write(" ctx="));
    trace_do(serial_write_hex64((uint64_t)context_id));
    trace_do(serial_write(" user="));
    trace_do(serial_write_hex64(user_addr));
    trace_do(serial_write(" size="));
    trace_do(serial_write_hex64(size));
    trace_do(serial_write(" root_expected="));
    trace_do(serial_write_hex64(root_expected));
    trace_do(serial_write(" root_current="));
    trace_do(serial_write_hex64(root_current));
    trace_do(serial_write(" chunk_user="));
    trace_do(serial_write_hex64(chunk_user_addr));
    trace_do(serial_write(" chunk_size="));
    trace_do(serial_write_hex64(chunk_size));
    trace_do(serial_write("\n"));
}

static int
mm_ensure_user_range_mapped(mm_context_t *ctx, uint64_t user_addr, uint64_t size, uint32_t needed_flags)
{
    if (!ctx || ctx->root_table == 0 || user_addr == 0 || size == 0) {
        return -1;
    }
    uint64_t end = user_addr + size;
    if (end < user_addr) {
        return -1;
    }
    uint64_t cur = user_addr;
    while (cur < end) {
        mem_region_t *region = mm_find_region_for_addr(ctx, cur);
        if (!region) {
            return -1;
        }
        if (!(region->flags & MEM_REGION_FLAG_USER) ||
            !(region->flags & MEM_REGION_FLAG_READ) ||
            ((needed_flags & MEM_REGION_FLAG_WRITE) && !(region->flags & MEM_REGION_FLAG_WRITE))) {
            return -1;
        }
        uint64_t page_base = cur & ~(PAGE_SIZE - 1ULL);
        uint64_t phys_page = region->phys_base + (page_base - region->base);
        if (paging_map_4k_in_root(ctx->root_table, page_base, phys_page, region->flags) < 0) {
            return -1;
        }
        cur = page_base + PAGE_SIZE;
    }
    return 0;
}

int
mm_user_range_permitted(uint32_t context_id, uint64_t user_addr, uint64_t size, uint32_t needed_flags)
{
    if (context_id == 0 || user_addr == 0 || size == 0) {
        return -1;
    }

    mm_context_t *ctx = mm_context_get(context_id);
    if (!ctx || ctx->root_table == 0) {
        return -1;
    }

    uint64_t end = user_addr + size;
    if (end < user_addr) {
        return -1;
    }

    uint64_t cur = user_addr;
    while (cur < end) {
        mem_region_t *region = mm_find_region_for_addr(ctx, cur);
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

int mm_handle_page_fault(uint32_t context_id, uint64_t addr, uint64_t error_code, uint64_t *out_mapped_base) {
    mm_context_t *ctx = mm_context_get(context_id);
    if (!ctx) {
        return -1;
    }

    if (error_code & pf_err_present) {
        return -1;
    }

    mem_region_t *region = mm_find_region_for_addr(ctx, addr);
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
    /* Region metadata now tracks a process-visible virtual base and a separate
     * physical backing base. Fault resolution wires the missing virtual page
     * into the owning context's private root table on demand. */
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

int mm_shared_create(uint32_t owner_context_id, uint64_t pages, uint32_t flags,
                     uint32_t *out_id, uint64_t *out_base) {
    if (!out_id || !out_base || pages == 0) {
        return -1;
    }
    uint32_t slot = MM_MAX_SHARED;
    for (uint32_t i = 0; i < MM_MAX_SHARED; ++i) {
        if (!g_shared[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == MM_MAX_SHARED) {
        return -1;
    }
    uint64_t base = pfa_alloc_pages(pages);
    if (!base) {
        return -1;
    }
    uint32_t id = g_shared_next_id++;
    if (id == 0) {
        id = g_shared_next_id++;
    }
    g_shared[slot].id = id;
    g_shared[slot].owner_context_id = owner_context_id;
    g_shared[slot].in_use = 1;
    g_shared[slot].refcount = 0;
    g_shared[slot].base = base;
    g_shared[slot].pages = pages;
    g_shared[slot].flags = flags;
    g_shared[slot].grant_count = 0;
    memset(g_shared[slot].grant_contexts, 0, sizeof(g_shared[slot].grant_contexts));
    *out_id = id;
    *out_base = base;
    return 0;
}

int mm_shared_grant(uint32_t owner_context_id, uint32_t id, uint32_t target_context_id) {
    mm_shared_region_t *region = mm_shared_find(id);
    uint8_t i = 0;
    if (!region || target_context_id == 0) {
        return -1;
    }
    if (owner_context_id != 0 && region->owner_context_id != owner_context_id) {
        return -1;
    }
    if (region->owner_context_id == target_context_id) {
        return 0;
    }
    for (i = 0; i < region->grant_count && i < MM_MAX_SHARED_GRANTS; ++i) {
        if (region->grant_contexts[i] == target_context_id) {
            return 0;
        }
    }
    if (region->grant_count >= MM_MAX_SHARED_GRANTS) {
        return -1;
    }
    region->grant_contexts[region->grant_count++] = target_context_id;
    return 0;
}

int mm_shared_get_phys(uint32_t owner_context_id, uint32_t id,
                       uint64_t *out_base, uint64_t *out_pages) {
    if (!out_base || !out_pages) {
        return -1;
    }
    mm_shared_region_t *region = mm_shared_find(id);
    if (!region || !mm_shared_access_allowed(region, owner_context_id)) {
        return -1;
    }
    *out_base = region->base;
    *out_pages = region->pages;
    return 0;
}

int mm_shared_retain(uint32_t owner_context_id, uint32_t id) {
    mm_shared_region_t *region = mm_shared_find(id);
    if (!region || !mm_shared_access_allowed(region, owner_context_id)) {
        return -1;
    }
    region->refcount++;
    return 0;
}

int mm_shared_release(uint32_t owner_context_id, uint32_t id) {
    mm_shared_region_t *region = mm_shared_find(id);
    if (!region || !mm_shared_access_allowed(region, owner_context_id) || region->refcount == 0) {
        return -1;
    }
    region->refcount--;
    return mm_shared_free_if_unused(region);
}

int mm_shared_map(mm_context_t *ctx, uint32_t id, uint32_t flags, uint64_t *out_base) {
    if (!ctx) {
        return -1;
    }
    mm_shared_region_t *region = mm_shared_find(id);
    if (!region || !mm_shared_access_allowed(region, ctx->id)) {
        return -1;
    }
    uint32_t effective_flags = region->flags;
    if (flags) {
        effective_flags &= flags;
    }
    uint64_t virt_base = mm_region_virtual_base(ctx, MEM_REGION_SHARED, region->pages);
    if (virt_base == 0 || mm_context_add_region(ctx, virt_base, region->pages * PAGE_SIZE,
                                                effective_flags, MEM_REGION_SHARED) != 0) {
        return -1;
    }
    ctx->regions[ctx->region_count - 1].phys_base = region->base;
    if (mm_shared_retain(ctx->id, id) != 0) {
        return -1;
    }
    if (out_base) {
        *out_base = virt_base;
    }
    return 0;
}

int mm_shared_unmap(mm_context_t *ctx, uint32_t id) {
    if (!ctx) {
        return -1;
    }
    mm_shared_region_t *region = mm_shared_find(id);
    if (!region || !mm_shared_access_allowed(region, ctx->id)) {
        return -1;
    }

    uint32_t found = 0;
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *r = &ctx->regions[i];
        if (r->type == MEM_REGION_SHARED && r->phys_base == region->base &&
            r->size == region->pages * PAGE_SIZE) {
            for (uint32_t j = i; j + 1 < ctx->region_count; ++j) {
                ctx->regions[j] = ctx->regions[j + 1];
            }
            ctx->region_count--;
            found = 1;
            break;
        }
    }
    if (!found) {
        return -1;
    }
    return mm_shared_release(ctx->id, id);
}

int mm_context_alloc_region(mm_context_t *ctx, uint64_t pages, uint32_t flags, mem_region_type_t type) {
    if (!ctx || pages == 0) {
        return -1;
    }
    uint64_t phys = pfa_alloc_pages(pages);
    if (!phys) {
        return -1;
    }
    uint64_t virt = mm_region_virtual_base(ctx, type, pages);
    if (!virt || mm_context_add_region(ctx, virt, pages * PAGE_SIZE, flags, type) != 0) {
        pfa_free_pages(phys, pages);
        return -1;
    }
    ctx->regions[ctx->region_count - 1].phys_base = phys;
    return 0;
}

mm_context_t *mm_context_get(uint32_t id) {
    if (id == g_root_ctx.id) {
        return &g_root_ctx;
    }
    mm_context_t *table = mm_context_table();
    uint32_t count = *mm_context_count_ptr();
    for (uint32_t i = 0; i < count; ++i) {
        if (table[i].id == id) {
            return &table[i];
        }
    }
    return 0;
}

mm_context_t *mm_context_create(uint32_t id) {
    if (id == g_root_ctx.id) {
        return &g_root_ctx;
    }
    uint32_t *count_ptr = mm_context_count_ptr();
    mm_context_t *table = mm_context_table();
    if (*count_ptr >= MM_MAX_CONTEXTS) {
        return 0;
    }
    mm_context_t *ctx = &table[*count_ptr];
    if (mm_context_init(ctx, id) != 0) {
        return 0;
    }
    if (paging_create_address_space(&ctx->root_table) != 0) {
        return 0;
    }
    if (mm_context_alloc_region(ctx, 8, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER,
                                MEM_REGION_WASM_LINEAR) != 0) {
        return 0;
    }
    if (mm_context_alloc_region(ctx, 2, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER,
                                MEM_REGION_STACK) != 0) {
        return 0;
    }
    if (mm_context_alloc_region(ctx, 4, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER,
                                MEM_REGION_HEAP) != 0) {
        return 0;
    }
    if (paging_verify_user_root(ctx->root_table, 1) != 0) {
        serial_write("[mm] verify user root failed\n");
        paging_dump_user_root_kernel_mappings(ctx->root_table);
        return 0;
    }
    (*count_ptr)++;
    return ctx;
}

int mm_context_destroy(uint32_t id) {
    if (id == g_root_ctx.id || id == 0) {
        return -1;
    }
    uint32_t *count_ptr = mm_context_count_ptr();
    mm_context_t *table = mm_context_table();
    for (uint32_t i = 0; i < *count_ptr; ++i) {
        mm_context_t *ctx = &table[i];
        if (ctx->id != id) {
            continue;
        }
        for (uint32_t r = 0; r < ctx->region_count; ++r) {
            mem_region_t *region = &ctx->regions[r];
            if (region->phys_base == 0 || region->size == 0) {
                continue;
            }
            uint64_t pages = (region->size + PAGE_SIZE - 1ULL) / PAGE_SIZE;
            pfa_free_pages(region->phys_base, pages);
        }
        paging_destroy_address_space(ctx->root_table);
        if (i + 1 < *count_ptr) {
            table[i] = table[*count_ptr - 1];
        }
        (*count_ptr)--;
        return 0;
    }
    return -1;
}

int mm_context_activate(uint32_t id) {
    mm_context_t *ctx = mm_context_get(id);
    if (!ctx || ctx->root_table == 0) {
        return -1;
    }
    /* Scheduling flips CR3 between the kernel root context and the selected
     * process context. The kernel mappings stay shared; the private user slot
     * changes with the owning mm_context. */
    return paging_switch_root(ctx->root_table);
}

uint64_t mm_context_root_table(uint32_t id) {
    mm_context_t *ctx = mm_context_get(id);
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
    uint8_t *kernel_ptr;
} mm_copy_from_user_args_t;

typedef struct {
    uint32_t context_id;
    uint64_t user_addr;
    uint64_t size;
    uint64_t root_table;
    uint64_t prev_root;
    const uint8_t *kernel_ptr;
} mm_copy_to_user_args_t;

static int
mm_copy_from_user_impl(void *opaque)
{
    mm_copy_from_user_args_t *args = (mm_copy_from_user_args_t *)opaque;
    if (!args) {
        return -1;
    }
    uint8_t *dst_bytes = args->kernel_ptr;
    uint64_t remaining = args->size;
    uint64_t user_cur = args->user_addr;
    const uint64_t chunk_size = 256ULL;
    uint8_t bounce[256];

    while (remaining > 0) {
        uint64_t n = (remaining < chunk_size) ? remaining : chunk_size;
        if (paging_switch_root(args->root_table) != 0) {
            mm_trace_copy_fail("from", "switch_to_user", args->context_id, args->user_addr, args->size, args->root_table, paging_get_current_root_table(), user_cur, n);
            return -1;
        }
        memcpy(bounce, (const void *)(uintptr_t)user_cur, (size_t)n);
        if (paging_switch_root(args->prev_root) != 0) {
            mm_trace_copy_fail("from", "switch_to_prev", args->context_id, args->user_addr, args->size, args->prev_root, paging_get_current_root_table(), user_cur, n);
            return -1;
        }
        memcpy(dst_bytes, bounce, (size_t)n);
        dst_bytes += n;
        user_cur += n;
        remaining -= n;
    }
    return 0;
}

int
mm_copy_from_user(uint32_t context_id, void *dst, uint64_t user_src, uint64_t size)
{
    if (context_id == 0 || !dst || user_src == 0 || size == 0) {
        mm_trace_copy_fail("from", "arg", context_id, user_src, size, 0, paging_get_current_root_table(), user_src, 0);
        return -1;
    }
    mm_context_t *ctx = mm_context_get(context_id);
    if (!ctx || ctx->root_table == 0) {
        mm_trace_copy_fail("from", "ctx", context_id, user_src, size, 0, paging_get_current_root_table(), user_src, 0);
        return -1;
    }
    if (mm_ensure_user_range_mapped(ctx, user_src, size, MEM_REGION_FLAG_READ) != 0) {
        mm_trace_copy_fail("from", "map", context_id, user_src, size, ctx->root_table, paging_get_current_root_table(), user_src, size);
        return -1;
    }

    mm_copy_from_user_args_t args = {
        .context_id = context_id,
        .user_addr = user_src,
        .size = size,
        .root_table = ctx->root_table,
        .prev_root = paging_get_current_root_table(),
        .kernel_ptr = (uint8_t *)dst,
    };
    return mm_run_on_copy_stack(mm_copy_from_user_impl, &args);
}

static int
mm_copy_to_user_impl(void *opaque)
{
    mm_copy_to_user_args_t *args = (mm_copy_to_user_args_t *)opaque;
    if (!args) {
        return -1;
    }
    const uint8_t *src_bytes = args->kernel_ptr;
    uint64_t remaining = args->size;
    uint64_t user_cur = args->user_addr;
    const uint64_t chunk_size = 256ULL;
    uint8_t bounce[256];

    while (remaining > 0) {
        uint64_t n = (remaining < chunk_size) ? remaining : chunk_size;
        memcpy(bounce, src_bytes, (size_t)n);
        if (paging_switch_root(args->root_table) != 0) {
            mm_trace_copy_fail("to", "switch_to_user", args->context_id, args->user_addr, args->size, args->root_table, paging_get_current_root_table(), user_cur, n);
            return -1;
        }
        memcpy((void *)(uintptr_t)user_cur, bounce, (size_t)n);
        if (paging_switch_root(args->prev_root) != 0) {
            mm_trace_copy_fail("to", "switch_to_prev", args->context_id, args->user_addr, args->size, args->prev_root, paging_get_current_root_table(), user_cur, n);
            return -1;
        }
        src_bytes += n;
        user_cur += n;
        remaining -= n;
    }
    return 0;
}

int
mm_copy_to_user(uint32_t context_id, uint64_t user_dst, const void *src, uint64_t size)
{
    if (context_id == 0 || user_dst == 0 || !src || size == 0) {
        mm_trace_copy_fail("to", "arg", context_id, user_dst, size, 0, paging_get_current_root_table(), user_dst, 0);
        return -1;
    }
    mm_context_t *ctx = mm_context_get(context_id);
    if (!ctx || ctx->root_table == 0) {
        mm_trace_copy_fail("to", "ctx", context_id, user_dst, size, 0, paging_get_current_root_table(), user_dst, 0);
        return -1;
    }
    if (mm_ensure_user_range_mapped(ctx, user_dst, size, MEM_REGION_FLAG_WRITE) != 0) {
        mm_trace_copy_fail("to", "map", context_id, user_dst, size, ctx->root_table, paging_get_current_root_table(), user_dst, size);
        return -1;
    }

    mm_copy_to_user_args_t args = {
        .context_id = context_id,
        .user_addr = user_dst,
        .size = size,
        .root_table = ctx->root_table,
        .prev_root = paging_get_current_root_table(),
        .kernel_ptr = (const uint8_t *)src,
    };
    return mm_run_on_copy_stack(mm_copy_to_user_impl, &args);
}

/* TODO: Migrate pointer-bearing syscall and IPC entry paths to this helper
 * family so kernel code stops dereferencing user virtual addresses directly. */

int mm_context_map_physical(uint32_t context_id,
                           uint64_t virt,
                           uint64_t phys,
                           uint64_t size,
                           uint32_t flags)
{
    if (context_id == 0 || virt == 0 || phys == 0 || size == 0) {
        return -1;
    }
    if ((virt & 0xFFFULL) != 0 || (phys & 0xFFFULL) != 0 || (size & 0xFFFULL) != 0) {
        return -1;
    }
    if (!mm_region_flags_valid(flags)) {
        return -1;
    }

    mm_context_t *ctx = mm_context_get(context_id);
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
        if (paging_map_4k_in_root(ctx->root_table, current_virt, current_phys, flags) != 0) {
            return -1;
        }
        current_virt += PAGE_SIZE;
        current_phys += PAGE_SIZE;
    }
    return 0;
}
