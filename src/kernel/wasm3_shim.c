#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include "serial.h"
#include "process.h"
#include "physmem.h"
#include "wasm3_shim.h"

#define WASM3_HEAP_DEFAULT_PAGES 1024u
#define WASM3_HEAP_MIN_PAGES 32u
#define WASM3_HEAP_ALIGN 16u
#define WASM3_HEAP_MAX_BYTES (2ULL * 1024ULL * 1024ULL * 1024ULL)
#define WASM3_HEAP_MAX_CHUNKS 64u

typedef struct {
    uint8_t *base;
    size_t size;
    size_t offset;
} wasm3_heap_chunk_t;

typedef struct {
    uint32_t pid;
    size_t preferred_chunk_size;
    size_t max_size;
    size_t committed_size;
    uint32_t chunk_count;
    /* The allocator grows by appending chunks. Allocation only advances the
     * tail chunk, which keeps the implementation simple and deterministic while
     * still allowing large heaps without one giant contiguous reservation. */
    wasm3_heap_chunk_t chunks[WASM3_HEAP_MAX_CHUNKS];
} wasm3_heap_slot_t;

typedef struct {
    size_t size;
    size_t total;
    size_t start;
} wasm3_heap_block_t;

static wasm3_heap_slot_t g_wasm3_heaps[PROCESS_MAX_COUNT];
static uint32_t g_wasm3_heap_bound_pid;

static void
wasm3_log_hex64(uint64_t value)
{
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

static size_t
align_up(size_t value, size_t align)
{
    if (align == 0) {
        return value;
    }
    size_t mask = align - 1;
    return (value + mask) & ~mask;
}

static void
wasm3_heap_slot_init(wasm3_heap_slot_t *slot, uint32_t pid)
{
    if (!slot) {
        return;
    }
    slot->pid = pid;
    slot->preferred_chunk_size = (size_t)WASM3_HEAP_DEFAULT_PAGES * 4096u;
    slot->max_size = (size_t)WASM3_HEAP_MAX_BYTES;
    slot->committed_size = 0;
    slot->chunk_count = 0;
    for (uint32_t i = 0; i < WASM3_HEAP_MAX_CHUNKS; ++i) {
        slot->chunks[i].base = 0;
        slot->chunks[i].size = 0;
        slot->chunks[i].offset = 0;
    }
}

static void
wasm3_memset(void *dst, int value, size_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < len; ++i) {
        p[i] = (uint8_t)value;
    }
}

static void
wasm3_memcpy(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < len; ++i) {
        d[i] = s[i];
    }
}

static wasm3_heap_slot_t *
wasm3_heap_slot_for_pid(uint32_t pid)
{
    if (pid == 0) {
        return 0;
    }
    wasm3_heap_slot_t *empty = 0;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm3_heaps[i].pid == pid) {
            return &g_wasm3_heaps[i];
        }
        if (!empty && g_wasm3_heaps[i].pid == 0) {
            empty = &g_wasm3_heaps[i];
        }
    }
    if (empty) {
        wasm3_heap_slot_init(empty, pid);
    }
    return empty;
}

static wasm3_heap_slot_t *
wasm3_heap_slot(void)
{
    uint32_t pid = g_wasm3_heap_bound_pid ? g_wasm3_heap_bound_pid : process_current_pid();
    return wasm3_heap_slot_for_pid(pid);
}

uint32_t
wasm3_heap_bind_pid(uint32_t pid)
{
    uint32_t previous_pid = g_wasm3_heap_bound_pid;
    g_wasm3_heap_bound_pid = pid;
    return previous_pid;
}

void
wasm3_heap_restore_pid(uint32_t previous_pid)
{
    g_wasm3_heap_bound_pid = previous_pid;
}

void
wasm3_heap_configure(uint32_t pid, uint64_t initial_size, uint64_t max_size)
{
    if (pid == 0) {
        return;
    }
    critical_section_enter();
    wasm3_heap_slot_t *slot = wasm3_heap_slot_for_pid(pid);
    if (slot) {
        size_t preferred = (size_t)initial_size;
        size_t limit = max_size == 0 ? (size_t)WASM3_HEAP_MAX_BYTES : (size_t)max_size;
        size_t minimum = (size_t)WASM3_HEAP_MIN_PAGES * 4096u;
        if (preferred == 0) {
            preferred = (size_t)WASM3_HEAP_DEFAULT_PAGES * 4096u;
        }
        if (preferred < minimum) {
            preferred = minimum;
        }
        if (limit < minimum) {
            limit = minimum;
        }
        if (limit > (size_t)WASM3_HEAP_MAX_BYTES) {
            limit = (size_t)WASM3_HEAP_MAX_BYTES;
        }
        if (preferred > limit) {
            preferred = limit;
        }
        slot->preferred_chunk_size = preferred;
        slot->max_size = limit;
    }
    critical_section_leave();
}

static int
wasm3_heap_grow(wasm3_heap_slot_t *slot, size_t min_total)
{
    if (!slot || min_total == 0) {
        return -1;
    }
    if (slot->chunk_count >= WASM3_HEAP_MAX_CHUNKS) {
        return -1;
    }
    if (slot->committed_size >= slot->max_size) {
        return -1;
    }

    size_t remaining = slot->max_size - slot->committed_size;
    if (min_total > remaining) {
        return -1;
    }

    /* New chunks try to preserve the configured startup footprint and then grow
     * from the size of the previous chunk, capped by the per-process limit. */
    size_t target = slot->preferred_chunk_size;
    if (slot->chunk_count > 0) {
        size_t last_size = slot->chunks[slot->chunk_count - 1].size;
        if (last_size > target) {
            target = last_size;
        }
    }
    if (target < min_total) {
        target = min_total;
    }
    if (target > remaining) {
        target = remaining;
    }

    uint64_t min_pages = (uint64_t)((min_total + 4095u) / 4096u);
    if (min_pages < WASM3_HEAP_MIN_PAGES) {
        min_pages = WASM3_HEAP_MIN_PAGES;
    }

    uint64_t pages = (uint64_t)((target + 4095u) / 4096u);
    if (pages < min_pages) {
        pages = min_pages;
    }

    uint64_t phys = 0;
    while (pages >= min_pages) {
        phys = pfa_alloc_pages_below(pages, 0x100000000ULL);
        if (!phys) {
            phys = pfa_alloc_pages(pages);
        }
        if (phys) {
            wasm3_heap_chunk_t *chunk = &slot->chunks[slot->chunk_count++];
            chunk->base = (uint8_t *)(uintptr_t)phys;
            chunk->size = (size_t)pages * 4096u;
            chunk->offset = 0;
            slot->committed_size += chunk->size;
            return 0;
        }
        if (pages == min_pages) {
            break;
        }
        pages /= 2u;
        if (pages < min_pages) {
            pages = min_pages;
        }
    }
    return -1;
}

static wasm3_heap_chunk_t *
wasm3_heap_tail_chunk(wasm3_heap_slot_t *slot)
{
    if (!slot || slot->chunk_count == 0) {
        return 0;
    }
    return &slot->chunks[slot->chunk_count - 1];
}

static wasm3_heap_chunk_t *
wasm3_heap_chunk_for_ptr(wasm3_heap_slot_t *slot, const void *ptr, uint32_t *out_index)
{
    if (!slot || !ptr) {
        return 0;
    }
    uintptr_t addr = (uintptr_t)ptr;
    for (uint32_t i = 0; i < slot->chunk_count; ++i) {
        wasm3_heap_chunk_t *chunk = &slot->chunks[i];
        uintptr_t base = (uintptr_t)chunk->base;
        uintptr_t end = base + chunk->size;
        if (addr >= base && addr < end) {
            if (out_index) {
                *out_index = i;
            }
            return chunk;
        }
    }
    return 0;
}

static void
wasm3_heap_release_empty_tail_chunks(wasm3_heap_slot_t *slot)
{
    if (!slot) {
        return;
    }
    /* Empty tail chunks can be returned to the frame allocator immediately.
     * Interior holes are intentionally not compacted yet. */
    while (slot->chunk_count > 1) {
        wasm3_heap_chunk_t *chunk = &slot->chunks[slot->chunk_count - 1];
        if (!chunk->base || chunk->offset != 0) {
            break;
        }
        uint64_t pages = ((uint64_t)chunk->size + 4095ULL) / 4096ULL;
        pfa_free_pages((uint64_t)(uintptr_t)chunk->base, pages);
        slot->committed_size -= chunk->size;
        chunk->base = 0;
        chunk->size = 0;
        chunk->offset = 0;
        slot->chunk_count--;
    }
}

static void *
wasm3_alloc(size_t size, int zero)
{
    if (size == 0) {
        return 0;
    }

    wasm3_heap_slot_t *slot = wasm3_heap_slot();
    if (!slot) {
        return 0;
    }
    critical_section_enter();
    size_t total = align_up(sizeof(wasm3_heap_block_t) + size, WASM3_HEAP_ALIGN);
    wasm3_heap_chunk_t *chunk = wasm3_heap_tail_chunk(slot);
    if (!chunk || align_up(chunk->offset, WASM3_HEAP_ALIGN) + total > chunk->size) {
        if (wasm3_heap_grow(slot, total) != 0) {
            serial_write("[wasm3-heap] grow failed pid=");
            wasm3_log_hex64(slot->pid);
            serial_write("[wasm3-heap] req=");
            wasm3_log_hex64((uint64_t)size);
            serial_write("[wasm3-heap] committed=");
            wasm3_log_hex64((uint64_t)slot->committed_size);
            serial_write("[wasm3-heap] limit=");
            wasm3_log_hex64((uint64_t)slot->max_size);
            critical_section_leave();
            return 0;
        }
        chunk = wasm3_heap_tail_chunk(slot);
    }
    if (!chunk) {
        critical_section_leave();
        return 0;
    }

    size_t aligned_offset = align_up(chunk->offset, WASM3_HEAP_ALIGN);
    if (aligned_offset + total > chunk->size) {
        serial_write("[wasm3-heap] chunk oom pid=");
        wasm3_log_hex64(slot->pid);
        serial_write("[wasm3-heap] req=");
        wasm3_log_hex64((uint64_t)size);
        serial_write("[wasm3-heap] off=");
        wasm3_log_hex64((uint64_t)chunk->offset);
        serial_write("[wasm3-heap] total=");
        wasm3_log_hex64((uint64_t)chunk->size);
        critical_section_leave();
        return 0;
    }

    wasm3_heap_block_t *block = (wasm3_heap_block_t *)(chunk->base + aligned_offset);
    block->size = size;
    block->total = total;
    block->start = aligned_offset;
    void *ptr = (uint8_t *)block + sizeof(wasm3_heap_block_t);
    chunk->offset = aligned_offset + total;
    critical_section_leave();
    if (zero) {
        wasm3_memset(ptr, 0, size);
    }
    return ptr;
}

void *malloc(size_t size)
{
    return wasm3_alloc(size, 0);
}

void *calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0) {
        return 0;
    }
    size_t total = nmemb * size;
    if (size != 0 && total / size != nmemb) {
        return 0;
    }
    return wasm3_alloc(total, 1);
}

void free(void *ptr)
{
    if (!ptr) {
        return;
    }
    wasm3_heap_slot_t *slot = wasm3_heap_slot();
    if (!slot || slot->chunk_count == 0) {
        return;
    }
    uint32_t chunk_index = 0;
    wasm3_heap_chunk_t *chunk = wasm3_heap_chunk_for_ptr(slot, ptr, &chunk_index);
    if (!chunk) {
        return;
    }
    wasm3_heap_block_t *block = (wasm3_heap_block_t *)((uint8_t *)ptr - sizeof(wasm3_heap_block_t));

    critical_section_enter();
    /* Free remains stack-like: only the most recent allocation in the tail
     * chunk shrinks the live frontier. This matches the old allocator's
     * behavior while still allowing chunked growth. */
    if (chunk_index + 1 == slot->chunk_count &&
        block->start + block->total == chunk->offset) {
        chunk->offset = block->start;
        wasm3_heap_release_empty_tail_chunks(slot);
    }
    critical_section_leave();
}

void wasm3_heap_release(uint32_t pid)
{
    if (pid == 0) {
        return;
    }
    critical_section_enter();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        wasm3_heap_slot_t *slot = &g_wasm3_heaps[i];
        if (slot->pid != pid) {
            continue;
        }
        for (uint32_t chunk_index = 0; chunk_index < slot->chunk_count; ++chunk_index) {
            wasm3_heap_chunk_t *chunk = &slot->chunks[chunk_index];
            if (!chunk->base || chunk->size == 0) {
                continue;
            }
            uint64_t pages = ((uint64_t)chunk->size + 4095ULL) / 4096ULL;
            pfa_free_pages((uint64_t)(uintptr_t)chunk->base, pages);
        }
        wasm3_heap_slot_init(slot, 0);
        break;
    }
    critical_section_leave();
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        return 0;
    }
    wasm3_heap_block_t *block = (wasm3_heap_block_t *)((uint8_t *)ptr - sizeof(wasm3_heap_block_t));
    size_t old_size = block->size;
    wasm3_heap_slot_t *slot = wasm3_heap_slot();
    size_t new_total = align_up(sizeof(wasm3_heap_block_t) + size, WASM3_HEAP_ALIGN);
    uint32_t chunk_index = 0;
    wasm3_heap_chunk_t *chunk = wasm3_heap_chunk_for_ptr(slot, ptr, &chunk_index);
    if (slot && chunk) {
        critical_section_enter();
        /* In-place growth is only safe for the newest allocation in the active
         * tail chunk. All other cases fall back to allocate-copy-free. */
        if (chunk_index + 1 == slot->chunk_count &&
            block->start + block->total == chunk->offset) {
            if (block->start + new_total <= chunk->size) {
                chunk->offset = block->start + new_total;
                block->size = size;
                block->total = new_total;
                critical_section_leave();
                return ptr;
            }
        }
        critical_section_leave();
    }
    void *new_ptr = malloc(size);
    if (!new_ptr) {
        return 0;
    }
    size_t copy_size = old_size < size ? old_size : size;
    wasm3_memcpy(new_ptr, ptr, copy_size);
    free(ptr);
    return new_ptr;
}

int
wasm3_heap_probe_growth(size_t size)
{
    if (size == 0) {
        return -1;
    }
    uint8_t *ptr = (uint8_t *)malloc(size);
    if (!ptr) {
        return -1;
    }
    ptr[0] = 0xA5u;
    ptr[size - 1] = 0x5Au;
    if (ptr[0] != 0xA5u || ptr[size - 1] != 0x5Au) {
        free(ptr);
        return -1;
    }
    free(ptr);
    return 0;
}

static int
append_char(char *buf, size_t size, int *idx, char ch)
{
    if (*idx + 1 < (int)size) {
        buf[*idx] = ch;
    }
    (*idx)++;
    return 0;
}

static int
append_str(char *buf, size_t size, int *idx, const char *s)
{
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        append_char(buf, size, idx, *s++);
    }
    return 0;
}

static int
append_uint(char *buf, size_t size, int *idx, uint64_t value, int base, int prefix)
{
    char tmp[32];
    int n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value > 0 && n < (int)sizeof(tmp)) {
            uint64_t digit = value % (uint64_t)base;
            tmp[n++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
            value /= (uint64_t)base;
        }
    }
    if (prefix && base == 16) {
        append_char(buf, size, idx, '0');
        append_char(buf, size, idx, 'x');
    }
    for (int i = n - 1; i >= 0; --i) {
        append_char(buf, size, idx, tmp[i]);
    }
    return 0;
}

static int
format_to_buffer(char *buf, size_t size, const char *fmt, va_list ap)
{
    int idx = 0;
    if (!buf || size == 0) {
        return 0;
    }
    if (!fmt) {
        buf[0] = '\0';
        return 0;
    }
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            append_char(buf, size, &idx, *p);
            continue;
        }
        ++p;
        if (*p == '%') {
            append_char(buf, size, &idx, '%');
            continue;
        }
        if (*p == 's') {
            append_str(buf, size, &idx, va_arg(ap, const char *));
            continue;
        }
        if (*p == 'd' || *p == 'i') {
            int v = va_arg(ap, int);
            if (v < 0) {
                append_char(buf, size, &idx, '-');
                append_uint(buf, size, &idx, (uint64_t)(-v), 10, 0);
            } else {
                append_uint(buf, size, &idx, (uint64_t)v, 10, 0);
            }
            continue;
        }
        if (*p == 'u') {
            unsigned int v = va_arg(ap, unsigned int);
            append_uint(buf, size, &idx, (uint64_t)v, 10, 0);
            continue;
        }
        if (*p == 'x') {
            unsigned int v = va_arg(ap, unsigned int);
            append_uint(buf, size, &idx, (uint64_t)v, 16, 0);
            continue;
        }
        if (*p == 'p') {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            append_uint(buf, size, &idx, (uint64_t)v, 16, 1);
            continue;
        }
        append_char(buf, size, &idx, '%');
        append_char(buf, size, &idx, *p);
    }
    if (idx >= (int)size) {
        buf[size - 1] = '\0';
    } else {
        buf[idx] = '\0';
    }
    return idx;
}

static FILE wasm3_stderr_instance;
FILE *stderr = &wasm3_stderr_instance;

int fprintf(FILE *stream, const char *fmt, ...)
{
    (void)stream;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int rc = format_to_buffer(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
    return rc;
}

int printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int rc = format_to_buffer(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
    return rc;
}

static unsigned long
parse_ul(const char *nptr, char **endptr, int base)
{
    if (base == 0) {
        base = 10;
    }
    unsigned long value = 0;
    const char *s = nptr;
    while (s && *s == ' ') {
        s++;
    }
    while (s && *s) {
        char c = *s;
        unsigned int digit = 0;
        if (c >= '0' && c <= '9') {
            digit = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned int)(10 + (c - 'a'));
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned int)(10 + (c - 'A'));
        } else {
            break;
        }
        if (digit >= (unsigned int)base) {
            break;
        }
        value = value * (unsigned long)base + (unsigned long)digit;
        s++;
    }
    if (endptr) {
        *endptr = (char *)(uintptr_t)s;
    }
    return value;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    return parse_ul(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    return (unsigned long long)parse_ul(nptr, endptr, base);
}
