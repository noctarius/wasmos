#ifndef WASMOS_FUTEX_H
#define WASMOS_FUTEX_H

#ifdef WASMOS_SCHED_THREADABLE

#include <stdint.h>

void futex_init(void);

/*
 * futex_wait — block calling thread if *uaddr == expected.
 * uaddr is a WASM linear-memory offset within context_id's address space.
 * timeout_ms == 0 means no timeout.
 * Returns 0 on wakeup, -1 on timeout, negative IPC error code on fault.
 */
int futex_wait(uint32_t uaddr, uint32_t expected,
               uint32_t timeout_ms, uint32_t context_id);

/*
 * futex_wake — wake up to count threads waiting on uaddr.
 * Returns the number of threads woken.
 */
int futex_wake(uint32_t uaddr, uint32_t count, uint32_t context_id);

#endif /* WASMOS_SCHED_THREADABLE */
#endif /* WASMOS_FUTEX_H */
