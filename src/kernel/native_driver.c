/* native_driver.c - Kernel-native (C) driver instance runner.
 * Spawns a native driver as a dedicated kernel thread with its own capability
 * profile and IPC endpoint, mirroring the WASM driver model but without the
 * wasm3 interpreter layer. */
#include "native_driver.h"
#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "physmem.h"
#include "process.h"
#include "thread.h"
#include "process_manager.h"
#include "serial.h"
#include "framebuffer.h"
#include "ipc.h"
#include "io.h"
#include "user_mutex.h"
#include "policy.h"
#include "wasmos_driver_abi.h"
#include "capability.h"
#include "timer.h"
#include "xfer_buffer.h"
#include <string.h>
#include <stddef.h>

/* Virtual base used for framebuffer mapping in native driver processes.
 * Matches MM_USER_DEVICE_BASE in memory.c — kept as a local constant to
 * avoid exposing memory-layout internals in the shared ABI header. */
#define ND_DEVICE_VIRT_BASE 0x0000008400000000ULL

/* Verify ABI structs are layout-compatible with the kernel types. */
_Static_assert(sizeof(nd_framebuffer_info_t) == sizeof(framebuffer_info_t),
               "nd_framebuffer_info_t size mismatch");
_Static_assert(sizeof(nd_ipc_message_t) == sizeof(ipc_message_t),
               "nd_ipc_message_t size mismatch");

/* ELF64 constants — local to avoid a dependency on the boot-time elf.h. */
#define ELF_MAG0    0x7fu
#define ELF_MAG1    'E'
#define ELF_MAG2    'L'
#define ELF_MAG3    'F'
#define ELFCLASS64  2
#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62
#define PT_LOAD     1
#define PF_W        2
#define PF_X        1

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

#define PAGE_SIZE 0x1000ULL
#define ND_HEAP_SLOTS PROCESS_MAX_COUNT

static uint32_t g_nd_heap_pid[ND_HEAP_SLOTS];
static uint64_t g_nd_heap_bytes[ND_HEAP_SLOTS];

static void
nd_heap_set(uint32_t pid, uint64_t bytes)
{
    uint32_t empty = ND_HEAP_SLOTS;
    if (pid == 0) {
        return;
    }
    critical_section_enter();
    for (uint32_t i = 0; i < ND_HEAP_SLOTS; ++i) {
        if (g_nd_heap_pid[i] == pid) {
            g_nd_heap_bytes[i] = bytes;
            critical_section_leave();
            return;
        }
        if (empty == ND_HEAP_SLOTS && g_nd_heap_pid[i] == 0) {
            empty = i;
        }
    }
    if (empty < ND_HEAP_SLOTS) {
        g_nd_heap_pid[empty] = pid;
        g_nd_heap_bytes[empty] = bytes;
    }
    critical_section_leave();
}

uint64_t
native_driver_heap_committed_bytes(uint32_t pid)
{
    uint64_t bytes = 0;
    if (pid == 0) {
        return 0;
    }
    critical_section_enter();
    for (uint32_t i = 0; i < ND_HEAP_SLOTS; ++i) {
        if (g_nd_heap_pid[i] == pid) {
            bytes = g_nd_heap_bytes[i];
            break;
        }
    }
    critical_section_leave();
    return bytes;
}

void
native_driver_heap_release(uint32_t pid)
{
    if (pid == 0) {
        return;
    }
    critical_section_enter();
    for (uint32_t i = 0; i < ND_HEAP_SLOTS; ++i) {
        if (g_nd_heap_pid[i] == pid) {
            g_nd_heap_pid[i] = 0;
            g_nd_heap_bytes[i] = 0;
            break;
        }
    }
    critical_section_leave();
}

/* -------------------------------------------------------------------------
 * API implementations
 * ---------------------------------------------------------------------- */

static int
nd_console_write(const char *ptr, int len)
{
    if (!ptr || len <= 0) {
        return -1;
    }
    char buf[128];
    int remaining = len;
    while (remaining > 0) {
        int chunk = remaining > (int)(sizeof(buf) - 1)
                        ? (int)(sizeof(buf) - 1)
                        : remaining;
        memcpy(buf, ptr + (len - remaining), (uint32_t)chunk);
        buf[chunk] = '\0';
        klog_write(buf);
        remaining -= chunk;
    }
    return 0;
}

static int
nd_console_read(char *ptr, int len)
{
    if (!ptr || len <= 0) {
        return -1;
    }
    int n = 0;
    while (n < len) {
        uint8_t c = 0;
        if (serial_read_char(&c) <= 0) {
            break;
        }
        ptr[n++] = (char)c;
    }
    return n;
}

static int
nd_framebuffer_info(nd_framebuffer_info_t *out)
{
    if (!out) {
        return -1;
    }
    /* framebuffer_info_t and nd_framebuffer_info_t are layout-identical
     * (verified by the _Static_assert above); cast is safe. */
    return framebuffer_get_info((framebuffer_info_t *)out);
}

static uint32_t
nd_sched_ticks(void)
{
    return (uint32_t)timer_ticks();
}

/* Per-driver borrow bookkeeping so buffer_release can reverse the exact
 * borrow and page mapping established by buffer_borrow. Native drivers borrow
 * at most a few buffers at a time (a framebuffer driver holds one for its
 * lifetime), so a small fixed table suffices. */
#define ND_BORROW_SLOTS 16

typedef struct {
    uint32_t driver_ctx;    /* 0 => free slot */
    uint32_t kind;
    uint32_t key_buffer_id; /* caller-supplied lookup key (0 for owner-local) */
    uint8_t  owner_local;   /* 1 => object was acquired here (framebuffer) */
    xfer_buffer_owner_t  owner;
    xfer_buffer_borrow_t borrow;
    uint64_t virt;
    uint64_t pages;
} nd_borrow_slot_t;

static nd_borrow_slot_t g_nd_borrows[ND_BORROW_SLOTS];

static nd_borrow_slot_t *
nd_borrow_find(uint32_t driver_ctx, uint32_t kind, uint32_t key_buffer_id)
{
    for (uint32_t i = 0; i < ND_BORROW_SLOTS; ++i) {
        if (g_nd_borrows[i].driver_ctx == driver_ctx &&
            g_nd_borrows[i].kind == kind &&
            g_nd_borrows[i].key_buffer_id == key_buffer_id) {
            return &g_nd_borrows[i];
        }
    }
    return (nd_borrow_slot_t *)0;
}

static nd_borrow_slot_t *
nd_borrow_alloc(void)
{
    for (uint32_t i = 0; i < ND_BORROW_SLOTS; ++i) {
        if (g_nd_borrows[i].driver_ctx == 0) {
            return &g_nd_borrows[i];
        }
    }
    return (nd_borrow_slot_t *)0;
}

static void
nd_borrow_slot_clear(nd_borrow_slot_t *slot)
{
    memset(slot, 0, sizeof(*slot));
}

static void
nd_log_invalid_buffer_borrow(uint32_t kind,
                             uint32_t source_context_id,
                             uint32_t buffer_id,
                             uint32_t flags,
                             uint32_t size)
{
    klog_printf("[native-driver] buffer_borrow invalid kind=%016llx src=%016llx buffer_id=%016llx flags=%016llx size=%016llx (size must be non-zero and page-aligned)\n",
                (unsigned long long)kind,
                (unsigned long long)source_context_id,
                (unsigned long long)buffer_id,
                (unsigned long long)flags,
                (unsigned long long)size);
}

static int
nd_map_pages(mm_context_t *ctx, uint64_t virt, uint64_t phys_base,
             uint64_t pages, uint32_t borrow_flags)
{
    uint32_t map_flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_USER;
    if (borrow_flags & BUFFER_BORROW_WRITE) {
        map_flags |= MEM_REGION_FLAG_WRITE;
    }
    for (uint64_t i = 0; i < pages; ++i) {
        (void)paging_unmap_4k_in_root(ctx->root_table, virt + i * PAGE_SIZE);
        if (paging_map_4k_in_root(ctx->root_table,
                                  virt + i * PAGE_SIZE,
                                  phys_base + i * PAGE_SIZE,
                                  map_flags) < 0) {
            for (uint64_t j = 0; j < i; ++j) {
                (void)paging_unmap_4k_in_root(ctx->root_table, virt + j * PAGE_SIZE);
            }
            return -1;
        }
    }
    return 0;
}

static void
nd_unmap_pages(mm_context_t *ctx, uint64_t virt, uint64_t pages)
{
    for (uint64_t i = 0; i < pages; ++i) {
        (void)paging_unmap_4k_in_root(ctx->root_table, virt + i * PAGE_SIZE);
    }
}

/*
 * Borrow a buffer object and map it into the driver's address space at
 * ND_DEVICE_VIRT_BASE. Owner-local borrows (framebuffer, or source_context_id
 * of 0/self) acquire the object here and borrow it from ourselves; foreign
 * borrows resolve buffer_id against source_context_id's owned objects. Low-level
 * ABI contract: size must be non-zero and page-aligned because the kernel maps
 * whole-page windows for native drivers.
 */
static void *
nd_buffer_borrow(uint32_t kind, uint32_t source_context_id,
                 uint32_t buffer_id, uint32_t flags, uint32_t size)
{
    if (size == 0 || (size & (uint32_t)(PAGE_SIZE - 1)) != 0) {
        nd_log_invalid_buffer_borrow(kind, source_context_id, buffer_id, flags, size);
        return (void *)0;
    }
    if (flags == 0 ||
        (flags & ~(uint32_t)(BUFFER_BORROW_READ | BUFFER_BORROW_WRITE)) != 0) {
        return (void *)0;
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        return (void *)0;
    }
    mm_context_t *ctx = mm_context_get(proc->context_id);
    if (!ctx || ctx->root_table == 0) {
        return (void *)0;
    }
    uint32_t driver_ctx = proc->context_id;

    if (nd_borrow_find(driver_ctx, kind, buffer_id)) {
        /* A borrow under this key is already active; release it first. */
        return (void *)0;
    }
    nd_borrow_slot_t *slot = nd_borrow_alloc();
    if (!slot) {
        return (void *)0;
    }

    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t borrow = {0};
    uint8_t owner_local = 0;

    if (kind == BUFFER_KIND_FRAMEBUFFER ||
        source_context_id == 0 || source_context_id == driver_ctx) {
        /* Owner-local: acquire the object here and borrow it from ourselves.
         * Framebuffers are always local-only (backed by the hardware fb). */
        if (xfer_buffer_acquire(kind, driver_ctx, size, &owner) != XFER_BUFFER_OK) {
            return (void *)0;
        }
        owner_local = 1;
    } else {
        /* Borrow a buffer object owned by a foreign context by its id. */
        xfer_buffer_t desc = {0};
        if (xfer_buffer_describe(buffer_id, kind, source_context_id, &desc) != XFER_BUFFER_OK) {
            return (void *)0;
        }
        if (xfer_buffer_get_owned(&desc, source_context_id, &owner) != XFER_BUFFER_OK) {
            return (void *)0;
        }
    }

    if (size > owner.buffer.size_bytes) {
        if (owner_local) {
            (void)xfer_buffer_release_owned(&owner);
        }
        return (void *)0;
    }

    if (xfer_buffer_borrow(&owner, driver_ctx, flags, &borrow) != XFER_BUFFER_OK) {
        if (owner_local) {
            (void)xfer_buffer_release_owned(&owner);
        }
        return (void *)0;
    }

    uint64_t phys_base = xfer_buffer_object_phys(&borrow.buffer);
    if (phys_base == 0) {
        (void)xfer_buffer_unborrow(&borrow);
        if (owner_local) {
            (void)xfer_buffer_release_owned(&owner);
        }
        return (void *)0;
    }

    uint64_t virt = ND_DEVICE_VIRT_BASE;
    uint64_t pages = (uint64_t)size / PAGE_SIZE;
    if (nd_map_pages(ctx, virt, phys_base, pages, flags) != 0) {
        (void)xfer_buffer_unborrow(&borrow);
        if (owner_local) {
            (void)xfer_buffer_release_owned(&owner);
        }
        return (void *)0;
    }

    slot->driver_ctx = driver_ctx;
    slot->kind = kind;
    slot->key_buffer_id = buffer_id;
    slot->owner_local = owner_local;
    slot->owner = owner;
    slot->borrow = borrow;
    slot->virt = virt;
    slot->pages = pages;
    return (void *)(uintptr_t)virt;
}

static int
nd_buffer_release(uint32_t kind, uint32_t buffer_id)
{
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        return -1;
    }
    uint32_t driver_ctx = proc->context_id;
    nd_borrow_slot_t *slot = nd_borrow_find(driver_ctx, kind, buffer_id);
    if (!slot) {
        return -1;
    }

    mm_context_t *ctx = mm_context_get(driver_ctx);
    if (ctx && ctx->root_table != 0) {
        nd_unmap_pages(ctx, slot->virt, slot->pages);
    }

    int rc = 0;
    if (xfer_buffer_unborrow(&slot->borrow) != XFER_BUFFER_OK) {
        rc = -1;
    }
    if (slot->owner_local &&
        xfer_buffer_release_owned(&slot->owner) != XFER_BUFFER_OK) {
        rc = -1;
    }
    nd_borrow_slot_clear(slot);
    return rc;
}

static int
nd_framebuffer_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    return framebuffer_put_pixel(x, y, color);
}

static int
nd_io_allowed(uint16_t port)
{
    process_t *proc = process_get(process_current_pid());
    if (!proc) {
        return 0;
    }
    return policy_authorize(proc->context_id, POLICY_ACTION_IO_PORT, port) == 0;
}

static uint8_t
nd_io_in8(uint16_t port)
{
    return nd_io_allowed(port) ? inb(port) : 0xFF;
}

static uint16_t
nd_io_in16(uint16_t port)
{
    return nd_io_allowed(port) ? inw(port) : 0xFFFF;
}

static void
nd_io_out8(uint16_t port, uint8_t val)
{
    if (nd_io_allowed(port)) {
        outb(port, val);
    }
}

static void
nd_io_out16(uint16_t port, uint16_t val)
{
    if (nd_io_allowed(port)) {
        outw(port, val);
    }
}

static uint32_t
nd_ipc_create_endpoint(void)
{
    process_t *proc = process_get(process_current_pid());
    if (!proc) {
        return IPC_ENDPOINT_NONE;
    }
    uint32_t ep = IPC_ENDPOINT_NONE;
    if (ipc_endpoint_create(proc->context_id, &ep) != 0) {
        return IPC_ENDPOINT_NONE;
    }
    return ep;
}

static int
nd_ipc_send(uint32_t sender_context_id, uint32_t endpoint,
            const nd_ipc_message_t *message)
{
    return ipc_send_from(sender_context_id, endpoint,
                         (const ipc_message_t *)message);
}

static int
nd_ipc_recv(uint32_t receiver_context_id, uint32_t endpoint,
            nd_ipc_message_t *out_message)
{
    return ipc_recv_for(receiver_context_id, endpoint,
                        (ipc_message_t *)out_message);
}

static void
nd_sched_yield(void)
{
    process_yield(PROCESS_RUN_IDLE);
}

static uint32_t
nd_sched_current_pid(void)
{
    return process_current_pid();
}

static uint32_t
nd_thread_current_tid(void)
{
    return thread_current_tid();
}

static int
nd_mutex_try_lock(uint64_t mutex_addr)
{
    process_t *proc = process_get(process_current_pid());
    if (!proc) {
        return -1;
    }
    return user_mutex_user_try_lock(proc->context_id,
                                    mutex_addr,
                                    thread_current_tid(),
                                    0);
}

static int
nd_mutex_unlock(uint64_t mutex_addr)
{
    process_t *proc = process_get(process_current_pid());
    if (!proc) {
        return -1;
    }
    return user_mutex_user_unlock(proc->context_id,
                                  mutex_addr,
                                  thread_current_tid(),
                                  0);
}

static uint32_t
nd_early_log_size(void)
{
    return serial_early_log_size();
}

static void
nd_early_log_copy(uint8_t *dst, uint32_t offset, uint32_t len)
{
    serial_early_log_copy(dst, offset, len);
}

static int
nd_shmem_create(uint64_t pages, uint32_t flags, uint32_t *out_id, void **out_ptr)
{
    uint64_t phys = 0;
    if (mm_shared_create(0, pages, flags, out_id, &phys) != 0) {
        return -1;
    }
    if (mm_shared_retain(0, *out_id) != 0) {
        (void)mm_shared_release(0, *out_id);
        return -1;
    }
    if (out_ptr) {
        *out_ptr = (void *)(uintptr_t)(phys | KERNEL_HIGHER_HALF_BASE);
    }
    return 0;
}

static void *
nd_shmem_map(uint32_t id)
{
    uint64_t base = 0;
    uint64_t pages = 0;
    if (mm_shared_get_phys(0, id, &base, &pages) != 0 || pages == 0) {
        return 0;
    }
    if (mm_shared_retain(0, id) != 0) {
        return 0;
    }
    return (void *)(uintptr_t)(base | KERNEL_HIGHER_HALF_BASE);
}

static int
nd_shmem_unmap(uint32_t id)
{
    return mm_shared_release(0, id);
}

static int
nd_shmem_flush(uint32_t id, const void *ptr, uint32_t size)
{
    uint64_t phys_base = 0;
    uint64_t pages = 0;
    uint32_t owner_context_id = 0;
    process_t *proc = process_get(process_current_pid());
    if (!ptr || size == 0) {
        return -1;
    }
    if (!proc || proc->context_id == 0) {
        return -1;
    }
    owner_context_id = proc->context_id;
    if (mm_shared_get_phys(owner_context_id, id, &phys_base, &pages) != 0 ||
        pages == 0 || phys_base == 0) {
        /* Fallback for legacy kernel-owned shared IDs. */
        if (mm_shared_get_phys(0, id, &phys_base, &pages) != 0 ||
            pages == 0 || phys_base == 0) {
            return -1;
        }
    }
    if (pages == 0 || phys_base == 0) {
        return -1;
    }
    if ((uint64_t)size > pages * PAGE_SIZE) {
        return -1;
    }
    memcpy((void *)(uintptr_t)(phys_base | KERNEL_HIGHER_HALF_BASE), ptr, (size_t)size);
    return 0;
}

static int
nd_shmem_grant(uint32_t id, uint32_t target_context_id)
{
    return mm_shared_grant(0, id, target_context_id);
}

static int
nd_ipc_endpoint_owner(uint32_t endpoint, uint32_t *out_owner_context_id)
{
    return ipc_endpoint_owner(endpoint, out_owner_context_id);
}

static uint32_t
nd_console_ring_id(void)
{
    return serial_console_ring_id();
}

static int
nd_console_register_fb(uint32_t context_id, uint32_t endpoint)
{
    (void)context_id;
    if (endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    process_manager_set_framebuffer_endpoint(endpoint);
    return 0;
}

static void
nd_proc_exit(int code)
{
    process_t *proc = process_get(process_current_pid());
    if (proc) {
        process_set_exit_status(proc, code);
    }
    process_yield(PROCESS_RUN_EXITED);
}

static void
nd_proc_notify_ready(void)
{
    process_manager_on_child_ready(process_current_pid());
}

/* -------------------------------------------------------------------------
 * ELF loader
 * ---------------------------------------------------------------------- */

static int
elf_validate_entry(const uint8_t *data, uint32_t size)
{
    const elf64_ehdr_t *hdr = (const elf64_ehdr_t *)data;
    for (uint16_t i = 0; i < hdr->e_phnum; ++i) {
        uint64_t ph_off = hdr->e_phoff + (uint64_t)i * sizeof(elf64_phdr_t);
        if (ph_off + sizeof(elf64_phdr_t) > (uint64_t)size) {
            return -1;
        }
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + ph_off);
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X) || ph->p_memsz == 0) {
            continue;
        }
        if (hdr->e_entry >= ph->p_vaddr &&
            hdr->e_entry <  ph->p_vaddr + ph->p_memsz) {
            return 0;
        }
    }
    return -1;
}

static int
elf_validate(const uint8_t *data, uint32_t size)
{
    if (size < sizeof(elf64_ehdr_t)) {
        return -1;
    }
    const elf64_ehdr_t *hdr = (const elf64_ehdr_t *)data;
    if (hdr->e_ident[0] != ELF_MAG0 || hdr->e_ident[1] != (uint8_t)ELF_MAG1 ||
        hdr->e_ident[2] != (uint8_t)ELF_MAG2 || hdr->e_ident[3] != (uint8_t)ELF_MAG3) {
        return -1;
    }
    if (hdr->e_ident[4] != ELFCLASS64) {
        return -1;
    }
    if (hdr->e_machine != EM_X86_64) {
        return -1;
    }
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        return -1;
    }
    if (hdr->e_phentsize != sizeof(elf64_phdr_t) || hdr->e_phnum == 0) {
        return -1;
    }
    return 0;
}

static int
copy_into_root(uint64_t root_table, uint64_t dst_virt, const void *src, uint64_t size)
{
    if (root_table == 0 || dst_virt == 0 || !src || size == 0) {
        return -1;
    }
    uint64_t prev_root = paging_get_current_root_table();
    const uint8_t *src_bytes = (const uint8_t *)src;
    uint64_t remaining = size;
    uint64_t dst_cur = dst_virt;
    const uint64_t chunk_size = 256ULL;
    uint8_t bounce[256];

    while (remaining > 0) {
        uint64_t n = (remaining < chunk_size) ? remaining : chunk_size;
        memcpy(bounce, src_bytes, (size_t)n);
        if (paging_switch_root(root_table) != 0) {
            (void)paging_switch_root(prev_root);
            return -1;
        }
        memcpy((void *)(uintptr_t)dst_cur, bounce, (size_t)n);
        if (paging_switch_root(prev_root) != 0) {
            return -1;
        }
        src_bytes += n;
        dst_cur += n;
        remaining -= n;
    }

    return 0;
}

static int
zero_into_root(uint64_t root_table, uint64_t dst_virt, uint64_t size)
{
    static const uint8_t zero_chunk[256] = {0};
    while (size > 0) {
        uint64_t chunk = size > sizeof(zero_chunk) ? sizeof(zero_chunk) : size;
        if (copy_into_root(root_table, dst_virt, zero_chunk, chunk) != 0) {
            return -1;
        }
        dst_virt += chunk;
        size -= chunk;
    }
    return 0;
}

typedef struct {
    uint64_t phys;
    uint64_t vpage;
    uint64_t pages;
} seg_alloc_t;

#define LOAD_SEG_MAX 64

static int
load_segments(const uint8_t *elf_data, uint32_t elf_size, uint64_t root_table, uint64_t *out_bytes)
{
    const elf64_ehdr_t *hdr = (const elf64_ehdr_t *)elf_data;
    uint64_t total_bytes = 0;
    seg_alloc_t loaded[LOAD_SEG_MAX];
    uint32_t n_loaded = 0;

    for (uint16_t i = 0; i < hdr->e_phnum; ++i) {
        uint64_t ph_off = hdr->e_phoff + (uint64_t)i * sizeof(elf64_phdr_t);
        if (ph_off + sizeof(elf64_phdr_t) > (uint64_t)elf_size) {
            goto fail;
        }
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(elf_data + ph_off);

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > (uint64_t)elf_size) {
            goto fail;
        }
        if (n_loaded >= LOAD_SEG_MAX) {
            goto fail;
        }

        /* Align the virtual base down to a page boundary. */
        uint64_t vpage = ph->p_vaddr & ~(PAGE_SIZE - 1ULL);
        uint64_t voff  = ph->p_vaddr - vpage;
        uint64_t alloc_pages = (ph->p_memsz + voff + PAGE_SIZE - 1ULL) / PAGE_SIZE;

        uint64_t phys = pfa_alloc_pages(alloc_pages);
        if (phys == 0) {
            goto fail;
        }

        /* Record before any fallible operations so fail path always sees it. */
        loaded[n_loaded].phys  = phys;
        loaded[n_loaded].vpage = vpage;
        loaded[n_loaded].pages = alloc_pages;
        ++n_loaded;

        uint32_t final_flags = MEM_REGION_FLAG_READ;
        if (ph->p_flags & PF_W) { final_flags |= MEM_REGION_FLAG_WRITE; }
        if (ph->p_flags & PF_X) { final_flags |= MEM_REGION_FLAG_EXEC;  }

        /* Always map writable first so memcpy can populate the pages.
         * Segments that don't request write will be remapped after the copy. */
        uint32_t copy_flags = final_flags | MEM_REGION_FLAG_WRITE;

        for (uint64_t p = 0; p < alloc_pages; ++p) {
            (void)paging_unmap_4k_in_root(root_table, vpage + p * PAGE_SIZE);
            if (paging_map_4k_in_root(root_table,
                                      vpage + p * PAGE_SIZE,
                                      phys  + p * PAGE_SIZE,
                                      copy_flags) < 0) {
                goto fail;
            }
        }

        const uint8_t *src = elf_data + ph->p_offset;

        if (ph->p_filesz > 0) {
            if (copy_into_root(root_table, ph->p_vaddr, src, ph->p_filesz) != 0) {
                goto fail;
            }
        }
        if (ph->p_memsz > ph->p_filesz) {
            if (zero_into_root(root_table,
                               ph->p_vaddr + ph->p_filesz,
                               ph->p_memsz - ph->p_filesz) != 0) {
                goto fail;
            }
        }

        /* Drop the temporary write permission for read-only/execute segments. */
        if (copy_flags != final_flags) {
            for (uint64_t p = 0; p < alloc_pages; ++p) {
                (void)paging_map_4k_in_root(root_table,
                                            vpage + p * PAGE_SIZE,
                                            phys  + p * PAGE_SIZE,
                                            final_flags);
            }
        }
        total_bytes += alloc_pages * PAGE_SIZE;
    }
    if (out_bytes) {
        *out_bytes = total_bytes;
    }
    return 0;

fail:
    for (uint32_t j = 0; j < n_loaded; ++j) {
        for (uint64_t p = 0; p < loaded[j].pages; ++p) {
            (void)paging_unmap_4k_in_root(root_table, loaded[j].vpage + p * PAGE_SIZE);
        }
        pfa_free_pages(loaded[j].phys, loaded[j].pages);
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------- */

int
native_driver_start(uint32_t context_id,
                    const uint8_t *elf_data, uint32_t elf_size,
                    const char *name,
                    const uint32_t *init_argv, uint32_t init_argc)
{
    uint32_t pid = process_current_pid();
    uint64_t loaded_bytes = 0;
    klog_write("[native-driver] loading ");
    klog_write(name ? name : "?");
    klog_write("\n");

    if (elf_validate(elf_data, elf_size) != 0) {
        klog_write("[native-driver] ELF validation failed\n");
        return -1;
    }

    mm_context_t *ctx = mm_context_get(context_id);
    if (!ctx || ctx->root_table == 0) {
        klog_write("[native-driver] no memory context\n");
        return -1;
    }

    if (load_segments(elf_data, elf_size, ctx->root_table, &loaded_bytes) != 0) {
        klog_write("[native-driver] segment load failed\n");
        return -1;
    }
    nd_heap_set(pid, loaded_bytes);

    const elf64_ehdr_t *hdr = (const elf64_ehdr_t *)elf_data;
    if (elf_validate_entry(elf_data, elf_size) != 0) {
        klog_write("[native-driver] ELF entry point outside executable segment\n");
        return -1;
    }
    native_driver_entry_fn_t entry =
        (native_driver_entry_fn_t)(uintptr_t)hdr->e_entry;

    wasmos_driver_api_t api;
    memset(&api, 0, sizeof(api));
    api.console_write       = nd_console_write;
    api.console_read        = nd_console_read;
    api.framebuffer_info    = nd_framebuffer_info;
    api.framebuffer_pixel   = nd_framebuffer_pixel;
    api.io_in8              = nd_io_in8;
    api.io_in16             = nd_io_in16;
    api.io_out8             = nd_io_out8;
    api.io_out16            = nd_io_out16;
    api.ipc_create_endpoint = nd_ipc_create_endpoint;
    api.ipc_send            = nd_ipc_send;
    api.ipc_recv            = nd_ipc_recv;
    api.sched_yield         = nd_sched_yield;
    api.sched_ticks         = nd_sched_ticks;
    api.sched_current_pid   = nd_sched_current_pid;
    api.thread_current_tid  = nd_thread_current_tid;
    api.mutex_try_lock      = nd_mutex_try_lock;
    api.mutex_unlock        = nd_mutex_unlock;
    api.proc_exit           = nd_proc_exit;
    api.proc_notify_ready   = nd_proc_notify_ready;
    api.early_log_size      = nd_early_log_size;
    api.early_log_copy      = nd_early_log_copy;
    api.shmem_create        = nd_shmem_create;
    api.shmem_grant         = nd_shmem_grant;
    api.shmem_map           = nd_shmem_map;
    api.shmem_unmap         = nd_shmem_unmap;
    api.ipc_endpoint_owner  = nd_ipc_endpoint_owner;
    api.console_ring_id     = nd_console_ring_id;
    api.console_register_fb = nd_console_register_fb;
    api.buffer_borrow       = nd_buffer_borrow;
    api.buffer_release      = nd_buffer_release;
    api.abi_magic           = WASMOS_NATIVE_ABI_MAGIC;
    api.abi_version         = WASMOS_NATIVE_ABI_VERSION;
    api.shmem_flush         = nd_shmem_flush;

    klog_write("[native-driver] calling initialize\n");
    /* The ELF is mapped only in the driver's address space (low VA, e.g.
     * IMAGE_BASE 0x10000000).  The kernel's page table has a bootstrap
     * identity mapping for 0..1 GB that points to different physical pages
     * than the ELF segments.  Switch to the driver's CR3 so that the entry
     * point resolves to the actual ELF code.  All kernel callback functions
     * are at higher-half VAs shared between every address space (PML4[511])
     * and remain reachable with the driver's CR3 active. */
    uint64_t kernel_cr3 = paging_get_current_root_table();
    if (paging_switch_root(ctx->root_table) != 0) {
        klog_write("[native-driver] CR3 switch to driver failed\n");
        return -1;
    }
    int rc = entry(&api,
                   (int)(init_argc > 0 ? init_argv[0] : 0),
                   (int)(init_argc > 1 ? init_argv[1] : 0),
                   (int)(init_argc > 2 ? init_argv[2] : 0));
    /* Restore kernel CR3 if entry() returned (error path or graceful exit
     * that did not go through process_yield(PROCESS_RUN_EXITED)). */
    (void)paging_switch_root(kernel_cr3);
    if (rc == -2) {
        klog_write("[native-driver] ABI mismatch\n");
    }
    klog_write("[native-driver] initialize returned\n");
    return rc;
}
