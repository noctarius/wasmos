/* kernel_ring3_smoke_runtime.h - Ring-3 isolation smoke tests.
 * Verifies that shared memory access controls and ring-3 process isolation work correctly.
 * Tests are run from the init process when WASMOS_RING3_SMOKE is enabled. */
#ifndef WASMOS_KERNEL_RING3_SMOKE_RUNTIME_H
#define WASMOS_KERNEL_RING3_SMOKE_RUNTIME_H

#include <stdint.h>

/* Assert that owner_context_id's shared memory is inaccessible to foreign_context_id. */
void kernel_shmem_owner_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id);

/* Run a matrix of shared memory misuse scenarios; ring3_mode gates ring-3-specific paths. */
void kernel_shmem_misuse_matrix_test(uint32_t owner_context_id, uint32_t foreign_context_id, uint8_t ring3_mode);

void kernel_ring3_shmem_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id);

/* Spawn a ring-3 smoke process under parent_pid; writes *out_pid on success. */
int kernel_ring3_spawn_smoke_process(uint32_t parent_pid, uint32_t *out_pid);

#endif
