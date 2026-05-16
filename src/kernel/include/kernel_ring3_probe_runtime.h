#ifndef WASMOS_KERNEL_RING3_PROBE_RUNTIME_H
#define WASMOS_KERNEL_RING3_PROBE_RUNTIME_H

#include <stdint.h>

int kernel_ring3_spawn_native_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_thread_lifecycle_probe(uint32_t parent_pid, uint32_t *out_pid);

int kernel_ring3_spawn_fault_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_write_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_exec_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_ud_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_gp_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_de_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_db_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_bp_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_of_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_nm_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_ss_probe(uint32_t parent_pid, uint32_t *out_pid);
int kernel_ring3_spawn_fault_ac_probe(uint32_t parent_pid, uint32_t *out_pid);

#endif
