#ifndef WASMOS_KERNEL_RING3_SMOKE_RUNTIME_H
#define WASMOS_KERNEL_RING3_SMOKE_RUNTIME_H

#include <stdint.h>

void kernel_shmem_owner_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id);
void kernel_shmem_misuse_matrix_test(uint32_t owner_context_id, uint32_t foreign_context_id, uint8_t ring3_mode);
void kernel_ring3_shmem_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id);
int kernel_ring3_spawn_smoke_process(uint32_t parent_pid, uint32_t *out_pid);

#endif
