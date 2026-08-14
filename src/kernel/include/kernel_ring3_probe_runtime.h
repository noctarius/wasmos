/* kernel_ring3_probe_runtime.h - Ring-3 smoke-probe launcher declarations. */
#ifndef WASMOS_KERNEL_RING3_PROBE_RUNTIME_H
#define WASMOS_KERNEL_RING3_PROBE_RUNTIME_H

#include <stdint.h>

/* Spawn a ring-3 probe process under parent_pid, writing its pid to *out_pid.  Each loads
 * a small hand-assembled payload into a fresh user address space and starts it parked, so
 * the caller can wait on the pid.  All return 0 on success and -1 on a NULL out_pid, a
 * missing payload, a failed spawn, or a failed mapping; on failure *out_pid may already
 * hold the pid of a spawned-but-unusable process.
 *
 * kernel_ring3_spawn_native_probe runs a payload that exercises the normal syscall path;
 * kernel_ring3_spawn_thread_lifecycle_probe exercises ring-3 thread create/exit. */
int kernel_ring3_spawn_native_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_thread_lifecycle_probe(uint32_t parent_pid, uint32_t* out_pid);

/* Fault probes: each payload deliberately raises exactly one exception from ring 3 and
 * must therefore be killed by the kernel rather than exiting.  In order: a #PF on a null
 * read, a #PF on a null write, a #PF on an instruction fetch from a non-executable page,
 * then #UD, #GP, #DE, #DB, #BP, #OF, #NM, #SS and #AC. */
int kernel_ring3_spawn_fault_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_write_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_exec_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_ud_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_gp_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_de_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_db_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_bp_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_of_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_nm_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_ss_probe(uint32_t parent_pid, uint32_t* out_pid);
int kernel_ring3_spawn_fault_ac_probe(uint32_t parent_pid, uint32_t* out_pid);

#endif
