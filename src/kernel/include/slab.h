#ifndef WASMOS_SLAB_H
#define WASMOS_SLAB_H

#include <stddef.h>

void slab_init(void);
void *kalloc_small(size_t size);
void kfree_small(void *ptr);

#endif
