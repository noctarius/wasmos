#ifndef WASMOS_FUTEX_H
#define WASMOS_FUTEX_H

#include <stdint.h>

/* Reset the futex wait table. Called once from process_init; every entry is
 * dropped, so it must not run while any thread is parked on a futex. */
void futex_init(void);

/*
 * futex_wait — block calling thread if *uaddr == expected.
 * uaddr is a WASM linear-memory offset within context_id's address space.
 * timeout_ms == 0 means no timeout.
 * Returns 0 on wakeup, or a negative IPC error code: IPC_ERR_TIMEOUT when the
 * deadline expired, IPC_ERR_INVALID for an unresolvable address, IPC_ERR_FULL
 * when the table is exhausted. The value reaches WASM through the futex_wait
 * hostcall, so every one of them is a generated code rather than a bare -1.
 *
 * 0 does NOT mean the word still equals `expected`, and it does not even mean
 * the caller blocked: a word that already differs returns 0 without parking,
 * and a spurious wake returns 0 as well. Always call it in a re-test loop.
 * Blocks otherwise, so it must not be called with a lock held or from a context
 * that cannot be descheduled.
 */
int futex_wait(uint32_t uaddr, uint32_t expected, uint32_t timeout_ms, uint32_t context_id);

/*
 * futex_wake — wake up to count threads waiting on uaddr.
 * Returns the number of threads woken, always >= 0 and never an error code: an
 * address that does not resolve and a word nobody waits on both report 0.
 * Waking is unconditional on the word's value -- publish the new value first,
 * since the woken waiters re-read it. Never blocks.
 */
int futex_wake(uint32_t uaddr, uint32_t count, uint32_t context_id);

#ifdef WASMOS_FUTEX_TEST_SEAMS
/* Host-test seam: drop every futex entry and empty the table. See futex.c. */
void futex_test_reset(void);
#endif

#endif /* WASMOS_FUTEX_H */