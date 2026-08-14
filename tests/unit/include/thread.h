/* Host shadow of src/kernel/include/thread.h.
 *
 * Only the identity a kernel synchronisation primitive reads is kept. The real
 * thread_t additionally carries scheduler, stack, context and process state
 * that no host test can populate, and pulling it in would drag process.h and
 * sched_list.h along with it. thread_current_tid is defined by the including
 * test. */
#ifndef WASMOS_TEST_THREAD_H
#define WASMOS_TEST_THREAD_H

#include <stdint.h>

typedef struct thread {
    uint32_t tid; /* Thread id; 0 is the "no thread" value the real tid space reserves. */
} thread_t;

/* Identity of the thread the primitive under test is running as. The real
 * implementation reads cpu_local()->current_thread and returns 0 when no thread
 * is current; a host definition is free to return whatever identity the case
 * needs, so a test can impersonate an arbitrary owner and can also produce a tid
 * the kernel never would. */
uint32_t thread_current_tid(void);

#endif
