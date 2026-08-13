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
    uint32_t tid;
} thread_t;

uint32_t thread_current_tid(void);

#endif
