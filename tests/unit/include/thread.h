#ifndef WASMOS_TEST_THREAD_H
#define WASMOS_TEST_THREAD_H

#include <stdint.h>

typedef struct thread {
    uint32_t tid;
} thread_t;

uint32_t thread_current_tid(void);

#endif
