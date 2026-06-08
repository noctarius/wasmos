#ifdef WASMOS_SCHED_THREADABLE

#include "futex.h"
#include "sched_event.h"
#include "sched.h"
#include "thread.h"
#include "ipc.h"
#include "memory.h"
#include "paging.h"
#include "spinlock.h"
#include "stdlib.h"
#include "string.h"

/*
 * futex.c — kernel futex implementation for the threadable scheduler.
 *
 * Futex words live in WASM linear memory.  The kernel key is the physical
 * address of the word, so different processes sharing the same page (via
 * shmem_grant) converge on the same futex_t bucket entry.
 *
 * Design mirrors Minos2's kernel/userspace/futex.c.
 */

#define FUTEX_TABLE_BITS 4
#define FUTEX_TABLE_SIZE (1u << FUTEX_TABLE_BITS)

typedef struct {
    uintptr_t     paddr;
    sched_event_t event;
    struct futex *next;
} futex_t;

static struct {
    spinlock_t lock;
    futex_t   *head;
} g_futex_table[FUTEX_TABLE_SIZE];

static inline uint32_t
futex_bucket(uintptr_t paddr)
{
    return (uint32_t)((paddr >> 12) & (FUTEX_TABLE_SIZE - 1u));
}

void
futex_init(void)
{
    for (uint32_t i = 0; i < FUTEX_TABLE_SIZE; i++) {
        spinlock_init(&g_futex_table[i].lock);
        g_futex_table[i].head = 0;
    }
}

static uintptr_t
futex_uaddr_to_paddr(uint32_t uaddr, uint32_t context_id)
{
    mm_context_t *ctx = mm_context_get(context_id);
    if (!ctx) {
        return 0;
    }
    mem_region_t wasm_region;
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &wasm_region) != 0) {
        return 0;
    }
    /* uaddr is an offset into WASM linear memory. */
    if ((uint64_t)uaddr + sizeof(uint32_t) > wasm_region.size) {
        return 0;
    }
    return (uintptr_t)(wasm_region.phys_base + (uint64_t)uaddr);
}

static futex_t *
futex_find(uintptr_t paddr, uint32_t bucket)
{
    futex_t *ft = g_futex_table[bucket].head;
    while (ft) {
        if (ft->paddr == paddr) {
            return ft;
        }
        ft = (futex_t *)ft->next;
    }
    return 0;
}

static futex_t *
futex_alloc(uintptr_t paddr, uint32_t bucket)
{
    futex_t *ft = (futex_t *)malloc(sizeof(futex_t));
    if (!ft) {
        return 0;
    }
    memset(ft, 0, sizeof(*ft));
    ft->paddr = paddr;
    sched_event_init(&ft->event, SCHED_EVENT_TYPE_FUTEX);
    ft->next = (struct futex *)g_futex_table[bucket].head;
    g_futex_table[bucket].head = ft;
    return ft;
}

int
futex_wait(uint32_t uaddr, uint32_t expected,
           uint32_t timeout_ms, uint32_t context_id)
{
    uintptr_t paddr = futex_uaddr_to_paddr(uaddr, context_id);
    if (!paddr) {
        return IPC_ERR_INVALID;
    }

    uint32_t bucket = futex_bucket(paddr);
    spinlock_lock(&g_futex_table[bucket].lock);

    futex_t *ft = futex_find(paddr, bucket);
    if (!ft) {
        ft = futex_alloc(paddr, bucket);
        if (!ft) {
            spinlock_unlock(&g_futex_table[bucket].lock);
            return IPC_ERR_FULL;
        }
    }

    /* Lock the event before releasing the bucket lock so no wake is missed. */
    spinlock_lock(&ft->event.lock);
    spinlock_unlock(&g_futex_table[bucket].lock);

    /* Re-read the futex word under the event lock to prevent the lost-wakeup
     * race: if the word already changed, return immediately. */
    uint32_t *kaddr = (uint32_t *)(uintptr_t)(paddr + KERNEL_HIGHER_HALF_BASE);
    if (*kaddr != expected) {
        spinlock_unlock(&ft->event.lock);
        return 0;
    }

    /* sched_event_wait releases ft->event.lock before yielding. */
    sched_event_wait(&ft->event, timeout_ms);

    thread_t *t = thread_get(thread_current_tid());
    if (t && t->pend_state == SCHED_PEND_TIMEOUT) {
        return -1;
    }
    return 0;
}

int
futex_wake(uint32_t uaddr, uint32_t count, uint32_t context_id)
{
    uintptr_t paddr = futex_uaddr_to_paddr(uaddr, context_id);
    if (!paddr) {
        return 0;
    }

    uint32_t bucket = futex_bucket(paddr);
    spinlock_lock(&g_futex_table[bucket].lock);

    futex_t *ft = futex_find(paddr, bucket);
    if (!ft) {
        spinlock_unlock(&g_futex_table[bucket].lock);
        return 0;
    }

    spinlock_lock(&ft->event.lock);
    spinlock_unlock(&g_futex_table[bucket].lock);

    int woken = 0;
    for (uint32_t i = 0; i < count; i++) {
        thread_t *t = sched_event_wake_one(&ft->event, 0, SCHED_PEND_OK);
        if (!t) {
            break;
        }
        woken++;
    }

    spinlock_unlock(&ft->event.lock);
    return woken;
}

#endif /* WASMOS_SCHED_THREADABLE */
