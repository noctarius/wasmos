#ifndef WASMOS_PHYSMEM_H
#define WASMOS_PHYSMEM_H

#include <stdint.h>
#include "boot.h"

void pfa_init(const boot_info_t *boot_info);
uint64_t pfa_alloc_pages(uint64_t pages);
void pfa_free_pages(uint64_t base, uint64_t pages);

#endif
