/* stdlib.c - WASM malloc/free: bump allocator with a free-list for reuse,
 * growing WASM memory pages on demand via __builtin_wasm_memory_grow */
#include "stdlib.h"

#include <stddef.h>
#include <stdint.h>
#include "string.h"

extern uint8_t __heap_base;

typedef struct heap_block {
    size_t size;
    int free;
    struct heap_block *next;
} heap_block_t;

static heap_block_t *g_heap_head = NULL;
static uint32_t g_heap_cursor = 0;
static uint32_t g_heap_limit = 0;
static int g_heap_ready = 0;

static size_t
heap_align(size_t v)
{
    const size_t a = sizeof(void *);
    return (v + (a - 1u)) & ~(a - 1u);
}

static void
heap_init(void)
{
    if (g_heap_ready) return;
    g_heap_limit = (uint32_t)__builtin_wasm_memory_size(0) * 65536u;
    g_heap_cursor = (uint32_t)(uintptr_t)&__heap_base;
    if (g_heap_cursor > g_heap_limit) g_heap_cursor = g_heap_limit;
    g_heap_head = NULL;
    g_heap_ready = 1;
}

/* Grow WASM linear memory in 64 KB pages until the limit covers need_end. */
static int
heap_grow_to(uint32_t need_end)
{
    while (need_end > g_heap_limit) {
        if (__builtin_wasm_memory_grow(0, 1) == (size_t)-1) return -1;
        g_heap_limit += 65536u;
    }
    return 0;
}

/* Bump-allocate a new block of at least payload_size bytes from the heap. */
static heap_block_t *
heap_request_block(size_t payload_size)
{
    if (payload_size > (size_t)(-1) - sizeof(heap_block_t)) return NULL;
    const size_t total = heap_align(sizeof(heap_block_t) + payload_size);
    if (total > 0xFFFFFFFFu) return NULL;
    uint32_t start = (g_heap_cursor + (uint32_t)(sizeof(void *) - 1u)) & ~(uint32_t)(sizeof(void *) - 1u);
    uint32_t end = start + (uint32_t)total;
    if (end < start) return NULL;
    if (heap_grow_to(end) != 0) return NULL;
    heap_block_t *blk = (heap_block_t *)(uintptr_t)start;
    blk->size = payload_size;
    blk->free = 0;
    blk->next = NULL;
    g_heap_cursor = end;
    return blk;
}

static void
heap_split_block(heap_block_t *blk, size_t want)
{
    if (!blk) return;
    const size_t aligned_want = heap_align(want);
    const size_t aligned_have = heap_align(blk->size);
    if (aligned_have <= aligned_want + sizeof(heap_block_t) + sizeof(void *)) return;
    uint8_t *base = (uint8_t *)(void *)blk;
    heap_block_t *next = (heap_block_t *)(void *)(base + sizeof(heap_block_t) + aligned_want);
    next->size = aligned_have - aligned_want - sizeof(heap_block_t);
    next->free = 1;
    next->next = blk->next;
    blk->size = aligned_want;
    blk->next = next;
}

static void
heap_coalesce(void)
{
    heap_block_t *cur = g_heap_head;
    while (cur && cur->next) {
        uint8_t *cur_end = (uint8_t *)(void *)cur + sizeof(heap_block_t) + heap_align(cur->size);
        if (cur->free && cur->next->free && cur_end == (uint8_t *)(void *)cur->next) {
            cur->size = heap_align(cur->size) + sizeof(heap_block_t) + heap_align(cur->next->size);
            cur->next = cur->next->next;
            continue;
        }
        cur = cur->next;
    }
}

void *
malloc(size_t size)
{
    if (size == 0) size = 1;
    heap_init();
    size = heap_align(size);

    heap_block_t *cur = g_heap_head;
    heap_block_t *prev = NULL;
    while (cur) {
        if (cur->free && cur->size >= size) {
            cur->free = 0;
            heap_split_block(cur, size);
            return (void *)((uint8_t *)(void *)cur + sizeof(heap_block_t));
        }
        prev = cur;
        cur = cur->next;
    }

    heap_block_t *blk = heap_request_block(size);
    if (!blk) return NULL;
    if (!g_heap_head) {
        g_heap_head = blk;
    } else {
        prev->next = blk;
    }
    return (void *)((uint8_t *)(void *)blk + sizeof(heap_block_t));
}

void
free(void *ptr)
{
    if (!ptr) return;
    uint8_t *p = (uint8_t *)ptr;
    heap_block_t *blk = (heap_block_t *)(void *)(p - sizeof(heap_block_t));
    blk->free = 1;
    heap_coalesce();
}

void *
calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0) return malloc(1);
    if (nmemb > ((size_t)-1) / size) return NULL;
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (!ptr) return NULL;
    memset(ptr, 0, total);
    return ptr;
}

void *
realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    heap_block_t *blk = (heap_block_t *)(void *)((uint8_t *)ptr - sizeof(heap_block_t));
    size = heap_align(size);
    if (blk->size >= size) {
        heap_split_block(blk, size);
        return ptr;
    }
    void *n = malloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, blk->size);
    free(ptr);
    return n;
}

static int
is_space(char ch)
{
    switch (ch) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '\f':
        case '\v':
            return 1;
        default:
            return 0;
    }
}

static int
digit_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'z') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'Z') {
        return 10 + (ch - 'A');
    }
    return -1;
}

int
abs(int value)
{
    if (value < 0) {
        return -value;
    }
    return value;
}

long
labs(long value)
{
    if (value < 0) {
        return -value;
    }
    return value;
}

long
strtol(const char *s, char **endptr, int base)
{
    const char *p = s;
    int negative = 0;
    int consumed = 0;

    if (!p) {
        if (endptr) {
            *endptr = NULL;
        }
        return 0;
    }

    while (is_space(*p)) {
        p++;
    }

    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
            p++;
        } else {
            base = 10;
        }
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    if (base < 2 || base > 36) {
        if (endptr) {
            *endptr = (char *)s;
        }
        return 0;
    }

    unsigned long uvalue = 0;
    int overflow = 0;
    /* LONG_MAX = 0x7FFF...F, LONG_MIN magnitude = 0x8000...0 */
    unsigned long pos_limit = (unsigned long)(-1L) >> 1;          /* LONG_MAX  */
    unsigned long neg_limit = pos_limit + 1u;                      /* LONG_MIN magnitude */

    while (*p) {
        int digit = digit_value(*p);
        if (digit < 0 || digit >= base) {
            break;
        }
        consumed = 1;
        if (!overflow) {
            unsigned long ubase = (unsigned long)base;
            if (uvalue > ((unsigned long)(-1L) - (unsigned long)digit) / ubase) {
                overflow = 1;
            } else {
                uvalue = uvalue * ubase + (unsigned long)digit;
                unsigned long limit = negative ? neg_limit : pos_limit;
                if (uvalue > limit) {
                    overflow = 1;
                }
            }
        }
        p++;
    }

    if (endptr) {
        *endptr = (char *)(consumed ? p : s);
    }

    if (overflow) {
        return negative ? (long)(~pos_limit) : (long)pos_limit;
    }
    return negative ? -(long)uvalue : (long)uvalue;
}

int
atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long
atol(const char *s)
{
    return strtol(s, NULL, 10);
}
