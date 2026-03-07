#ifndef WASMOS_SPINLOCK_H
#define WASMOS_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t state;
} spinlock_t;

void spinlock_init(spinlock_t *lock);
int spinlock_try_lock(spinlock_t *lock);
void spinlock_lock(spinlock_t *lock);
void spinlock_unlock(spinlock_t *lock);

#endif
