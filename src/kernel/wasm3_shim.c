#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include "serial.h"
#include "process.h"
#include "physmem.h"
#include "wasm3_shim.h"

#define WASM3_HEAP_PAGES 1024u
#define WASM3_HEAP_MIN_PAGES 32u
#define WASM3_HEAP_ALIGN 16u

typedef struct {
    uint32_t pid;
    uint8_t *base;
    size_t size;
    size_t offset;
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
wasm3_heap_slot(void)
{
    uint32_t pid = g_wasm3_heap_bound_pid ? g_wasm3_heap_bound_pid : process_current_pid();
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
        empty->pid = pid;
        empty->base = 0;
        empty->size = 0;
        empty->offset = 0;
    }
    return empty;
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
    if (!slot->base) {
        uint64_t min_pages = (uint64_t)((total + 4095u) / 4096u);
        if (min_pages < WASM3_HEAP_MIN_PAGES) {
            min_pages = WASM3_HEAP_MIN_PAGES;
        }

        uint64_t pages = WASM3_HEAP_PAGES;
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
                slot->base = (uint8_t *)(uintptr_t)phys;
                slot->size = (size_t)pages * 4096u;
                slot->offset = 0;
                break;
            }
            if (pages == min_pages) {
                break;
            }
            pages /= 2u;
            if (pages < min_pages) {
                pages = min_pages;
            }
        }
        if (!slot->base) {
            serial_write("[wasm3-heap] init alloc failed pid=");
            wasm3_log_hex64(process_current_pid());
            serial_write("[wasm3-heap] min_pages=");
            wasm3_log_hex64(min_pages);
            critical_section_leave();
            return 0;
        }
    }

    size_t aligned_offset = align_up(slot->offset, WASM3_HEAP_ALIGN);
    if (aligned_offset + total > slot->size) {
        serial_write("[wasm3-heap] oom pid=");
        wasm3_log_hex64(process_current_pid());
        serial_write("[wasm3-heap] req=");
        wasm3_log_hex64((uint64_t)size);
        serial_write("[wasm3-heap] off=");
        wasm3_log_hex64((uint64_t)slot->offset);
        serial_write("[wasm3-heap] total=");
        wasm3_log_hex64((uint64_t)slot->size);
        critical_section_leave();
        return 0;
    }

    wasm3_heap_block_t *block = (wasm3_heap_block_t *)(slot->base + aligned_offset);
    block->size = size;
    block->total = total;
    block->start = aligned_offset;
    void *ptr = (uint8_t *)block + sizeof(wasm3_heap_block_t);
    slot->offset = aligned_offset + total;
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
    if (!slot || !slot->base) {
        return;
    }
    wasm3_heap_block_t *block = (wasm3_heap_block_t *)((uint8_t *)ptr - sizeof(wasm3_heap_block_t));

    critical_section_enter();
    if (block->start + block->total == slot->offset) {
        slot->offset = block->start;
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
        if (slot->base && slot->size > 0) {
            uint64_t pages = ((uint64_t)slot->size + 4095ULL) / 4096ULL;
            pfa_free_pages((uint64_t)(uintptr_t)slot->base, pages);
        }
        slot->pid = 0;
        slot->base = 0;
        slot->size = 0;
        slot->offset = 0;
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
    if (slot && slot->base) {
        critical_section_enter();
        if (block->start + block->total == slot->offset) {
            if (block->start + new_total <= slot->size) {
                slot->offset = block->start + new_total;
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
