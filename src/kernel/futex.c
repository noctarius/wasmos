#include "futex.h"
#include "sched_event.h"
#include "sched.h"
#include "thread.h"
#include "ipc.h"
#include "memory.h"
#include "paging.h"
#include "sync/spinlock.h"
#include "stdlib.h"
#include "string.h"

/*
 * futex.c — kernel futex implementation on top of sched_event_t.
 *
 * Futex words live in WASM linear memory.  The kernel key is the physical
 * address of the word, so different processes sharing the same page (via
 * shmem_grant) converge on the same futex_t bucket entry.
 *
 * Design mirrors Minos2's kernel/userspace/futex.c.
 */

/* Hash table sizing.  16 buckets is a compromise: each bucket costs one spinlock
 * plus a list head, and collisions only cost a short walk plus contention on
 * that bucket's lock, never correctness. */
#define FUTEX_TABLE_BITS 4
#define FUTEX_TABLE_SIZE (1u << FUTEX_TABLE_BITS)

typedef struct {
    uintptr_t paddr;
    sched_event_t event;
    struct futex* next;
} futex_t;

static struct {
    ksync_spinlock_t lock;
    futex_t* head;
} g_futex_table[FUTEX_TABLE_SIZE];

/* Hashes on the PAGE number, not the word address, so every futex within one
 * page lands in the same bucket and shares its lock.  Cheap and adequate here
 * because a process's futexes are few; a workload with many hot futexes in one
 * page would serialise on that bucket. */
static inline uint32_t futex_bucket(uintptr_t paddr) {
    return (uint32_t)((paddr >> 12) & (FUTEX_TABLE_SIZE - 1u));
}

/* Publishes an empty table.  Call once, before any futex_wait/futex_wake, from
 * process_init(); it re-initialises every bucket lock, so running it against a
 * live table would leak the entries and strand their waiters. */
void futex_init(void) {
    for (uint32_t i = 0; i < FUTEX_TABLE_SIZE; i++) {
        ksync_spinlock_init(&g_futex_table[i].lock);
        g_futex_table[i].head = 0;
    }
}

static uintptr_t futex_uaddr_to_paddr(uint32_t uaddr, uint32_t context_id) {
    mm_context_t* ctx = mm_context_get(context_id);
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

#ifdef WASMOS_FUTEX_TEST_SEAMS
/*
 * Host-test seam.  Not compiled into the kernel, which has no teardown for
 * futex entries by design: an entry costs one allocation and is reused for the
 * life of the system.  A test that reuses thread slots across cases needs one,
 * or a stale wait_list keeps pointing at threads the next case has recycled.
 */
void futex_test_reset(void) {
    for (uint32_t i = 0; i < FUTEX_TABLE_SIZE; i++) {
        ksync_spinlock_lock(&g_futex_table[i].lock);
        futex_t* ft = g_futex_table[i].head;
        while (ft) {
            futex_t* next = (futex_t*)ft->next;
            free(ft);
            ft = next;
        }
        g_futex_table[i].head = 0;
        ksync_spinlock_unlock(&g_futex_table[i].lock);
    }
}
#endif

static futex_t* futex_find(uintptr_t paddr, uint32_t bucket) {
    futex_t* ft = g_futex_table[bucket].head;
    while (ft) {
        if (ft->paddr == paddr) {
            return ft;
        }
        ft = (futex_t*)ft->next;
    }
    return 0;
}

static futex_t* futex_alloc(uintptr_t paddr, uint32_t bucket) {
    futex_t* ft = (futex_t*)malloc(sizeof(futex_t));
    if (!ft) {
        return 0;
    }
    memset(ft, 0, sizeof(*ft));
    ft->paddr = paddr;
    sched_event_init(&ft->event, SCHED_EVENT_TYPE_FUTEX);
    ft->next = (struct futex*)g_futex_table[bucket].head;
    g_futex_table[bucket].head = ft;
    return ft;
}

int futex_wait(uint32_t uaddr, uint32_t expected, uint32_t timeout_ms, uint32_t context_id) {
    uintptr_t paddr = futex_uaddr_to_paddr(uaddr, context_id);
    if (!paddr) {
        return IPC_ERR_INVALID;
    }

    uint32_t bucket = futex_bucket(paddr);
    ksync_spinlock_lock(&g_futex_table[bucket].lock);

    futex_t* ft = futex_find(paddr, bucket);
    if (!ft) {
        ft = futex_alloc(paddr, bucket);
        if (!ft) {
            ksync_spinlock_unlock(&g_futex_table[bucket].lock);
            return IPC_ERR_FULL;
        }
    }

    /* Lock the event before releasing the bucket lock so no wake is missed. */
    ksync_spinlock_lock(&ft->event.lock);
    ksync_spinlock_unlock(&g_futex_table[bucket].lock);

    /* Re-read the futex word under the event lock to prevent the lost-wakeup
     * race: if the word already changed, return immediately.  Read through the
     * kernel's higher-half alias of the same physical page the guest writes, so
     * no user mapping has to be walked.  The load is plain rather than atomic:
     * a concurrent guest write is only guaranteed to be observed by the NEXT
     * pass, which is why the caller must treat a 0 return as "re-check the word
     * yourself" and not as "the word equals `expected`". */
    uint32_t* kaddr = ptr_cast(uint32_t, (paddr + KERNEL_HIGHER_HALF_BASE));
    if (*kaddr != expected) {
        ksync_spinlock_unlock(&ft->event.lock);
        return 0;
    }

    /* sched_event_wait releases ft->event.lock before yielding. */
    sched_event_wait(&ft->event, timeout_ms);

    thread_t* t = thread_get(thread_current_tid());
    if (t && t->pend_state == SCHED_PEND_TIMEOUT) {
        /* IPC_ERR_TIMEOUT, not -1: this value reaches the guest through the
         * futex_wait hostcall, and -1 is IPC_ERR_INVALID's value (ipc.h), which
         * would make an expired deadline indistinguishable from a bad address. */
        return IPC_ERR_TIMEOUT;
    }
    return 0;
}

/* Wakes up to `count` threads parked on the word at `uaddr` and returns how many
 * were actually woken — always >= 0, never a packed error code.  A word nobody
 * is waiting on, and an address that does not resolve in `context_id`, both
 * report 0: the caller cannot tell a bad address from an idle futex, which is
 * deliberate because neither is actionable at a wake site.  Never blocks.
 *
 * Waking is not conditional on the word's value: the caller is expected to have
 * published the new value before calling, and the woken waiters re-read it. */
int futex_wake(uint32_t uaddr, uint32_t count, uint32_t context_id) {
    uintptr_t paddr = futex_uaddr_to_paddr(uaddr, context_id);
    if (!paddr) {
        return 0;
    }

    uint32_t bucket = futex_bucket(paddr);
    ksync_spinlock_lock(&g_futex_table[bucket].lock);

    futex_t* ft = futex_find(paddr, bucket);
    if (!ft) {
        ksync_spinlock_unlock(&g_futex_table[bucket].lock);
        return 0;
    }

    ksync_spinlock_lock(&ft->event.lock);
    ksync_spinlock_unlock(&g_futex_table[bucket].lock);

    int woken = 0;
    for (uint32_t i = 0; i < count; i++) {
        thread_t* t = sched_event_wake_one(&ft->event, 0, SCHED_PEND_OK);
        if (!t) {
            break;
        }
        woken++;
    }

    ksync_spinlock_unlock(&ft->event.lock);
    return woken;
}
