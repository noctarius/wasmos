#include "native_driver.h"
#include "memory.h"
#include "paging.h"
#include "physmem.h"
#include "process.h"
#include "process_manager.h"
#include "serial.h"
#include "framebuffer.h"
#include "ipc.h"
#include "io.h"
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
        serial_write(buf);
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

/*
 * Map the physical framebuffer into the driver's address space at
 * ND_DEVICE_VIRT_BASE and return that virtual pointer.
 * The driver process has no pre-allocated device region; pages are mapped
 * directly into its root page table.
 */
static void *
nd_framebuffer_map(uint32_t size)
{
    if (size == 0 || (size & (uint32_t)(PAGE_SIZE - 1)) != 0) {
        return (void *)0;
    }

    framebuffer_info_t info;
    if (framebuffer_get_info(&info) != 0) {
        return (void *)0;
    }
    if ((uint64_t)size > info.framebuffer_size) {
        return (void *)0;
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc) {
        return (void *)0;
    }
    mm_context_t *ctx = mm_context_get(proc->context_id);
    if (!ctx || ctx->root_table == 0) {
        return (void *)0;
    }

    uint64_t virt = ND_DEVICE_VIRT_BASE;
    uint64_t phys = info.framebuffer_base;
    uint64_t pages = (uint64_t)size / PAGE_SIZE;

    for (uint64_t i = 0; i < pages; ++i) {
        (void)paging_unmap_4k_in_root(ctx->root_table, virt + i * PAGE_SIZE);
        if (paging_map_4k_in_root(ctx->root_table,
                                  virt + i * PAGE_SIZE,
                                  phys + i * PAGE_SIZE,
                                  MEM_REGION_FLAG_READ |
                                      MEM_REGION_FLAG_WRITE) < 0) {
            return (void *)0;
        }
    }
    return (void *)(uintptr_t)virt;
}

static int
nd_framebuffer_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    return framebuffer_put_pixel(x, y, color);
}

static uint8_t  nd_io_in8(uint16_t port)                  { return inb(port); }
static uint16_t nd_io_in16(uint16_t port)                 { return inw(port); }
static void     nd_io_out8(uint16_t port, uint8_t val)    { outb(port, val); }
static void     nd_io_out16(uint16_t port, uint16_t val)  { outw(port, val); }

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
        return -1;
    }
    if (out_ptr) {
        *out_ptr = (void *)(uintptr_t)phys;
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
    return (void *)(uintptr_t)base;
}

static int
nd_shmem_unmap(uint32_t id)
{
    return mm_shared_release(0, id);
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

/* -------------------------------------------------------------------------
 * ELF loader
 * ---------------------------------------------------------------------- */

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

static int
load_segments(const uint8_t *elf_data, uint32_t elf_size, uint64_t root_table)
{
    const elf64_ehdr_t *hdr = (const elf64_ehdr_t *)elf_data;

    for (uint16_t i = 0; i < hdr->e_phnum; ++i) {
        uint64_t ph_off = hdr->e_phoff + (uint64_t)i * sizeof(elf64_phdr_t);
        if (ph_off + sizeof(elf64_phdr_t) > (uint64_t)elf_size) {
            return -1;
        }
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(elf_data + ph_off);

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > (uint64_t)elf_size) {
            return -1;
        }

        /* Align the virtual base down to a page boundary. */
        uint64_t vpage = ph->p_vaddr & ~(PAGE_SIZE - 1ULL);
        uint64_t voff  = ph->p_vaddr - vpage;
        uint64_t alloc_pages = (ph->p_memsz + voff + PAGE_SIZE - 1ULL) / PAGE_SIZE;

        uint64_t phys = pfa_alloc_pages(alloc_pages);
        if (phys == 0) {
            return -1;
        }

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
                pfa_free_pages(phys, alloc_pages);
                return -1;
            }
        }

        const uint8_t *src = elf_data + ph->p_offset;

        if (ph->p_filesz > 0) {
            if (copy_into_root(root_table, ph->p_vaddr, src, ph->p_filesz) != 0) {
                pfa_free_pages(phys, alloc_pages);
                return -1;
            }
        }
        if (ph->p_memsz > ph->p_filesz) {
            if (zero_into_root(root_table,
                               ph->p_vaddr + ph->p_filesz,
                               ph->p_memsz - ph->p_filesz) != 0) {
                pfa_free_pages(phys, alloc_pages);
                return -1;
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
    }
    return 0;
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
    serial_write("[native-driver] loading ");
    serial_write(name ? name : "?");
    serial_write("\n");

    if (elf_validate(elf_data, elf_size) != 0) {
        serial_write("[native-driver] ELF validation failed\n");
        return -1;
    }

    mm_context_t *ctx = mm_context_get(context_id);
    if (!ctx || ctx->root_table == 0) {
        serial_write("[native-driver] no memory context\n");
        return -1;
    }

    if (load_segments(elf_data, elf_size, ctx->root_table) != 0) {
        serial_write("[native-driver] segment load failed\n");
        return -1;
    }

    const elf64_ehdr_t *hdr = (const elf64_ehdr_t *)elf_data;
    native_driver_entry_fn_t entry =
        (native_driver_entry_fn_t)(uintptr_t)hdr->e_entry;

    wasmos_driver_api_t api;
    api.console_write       = nd_console_write;
    api.console_read        = nd_console_read;
    api.framebuffer_info    = nd_framebuffer_info;
    api.framebuffer_map     = nd_framebuffer_map;
    api.framebuffer_pixel   = nd_framebuffer_pixel;
    api.io_in8              = nd_io_in8;
    api.io_in16             = nd_io_in16;
    api.io_out8             = nd_io_out8;
    api.io_out16            = nd_io_out16;
    api.ipc_create_endpoint = nd_ipc_create_endpoint;
    api.ipc_send            = nd_ipc_send;
    api.ipc_recv            = nd_ipc_recv;
    api.sched_yield         = nd_sched_yield;
    api.sched_current_pid   = nd_sched_current_pid;
    api.proc_exit           = nd_proc_exit;
    api.early_log_size      = nd_early_log_size;
    api.early_log_copy      = nd_early_log_copy;
    api.shmem_create        = nd_shmem_create;
    api.shmem_map           = nd_shmem_map;
    api.shmem_unmap         = nd_shmem_unmap;
    api.console_ring_id     = nd_console_ring_id;
    api.console_register_fb = nd_console_register_fb;

    serial_write("[native-driver] calling initialize\n");
    int rc = entry(&api,
                   (int)(init_argc > 0 ? init_argv[0] : 0),
                   (int)(init_argc > 1 ? init_argv[1] : 0),
                   (int)(init_argc > 2 ? init_argv[2] : 0));
    serial_write("[native-driver] initialize returned\n");
    return rc;
}
