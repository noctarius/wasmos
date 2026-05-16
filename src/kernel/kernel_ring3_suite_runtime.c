#include "kernel_ring3_suite_runtime.h"

#include "kernel_ring3_fault_runtime.h"
#include "kernel_ring3_probe_runtime.h"
#include "kernel_ring3_smoke_runtime.h"

#include "klog.h"
#include "process.h"

static int
spawn_ring3_fault_churn_probe_process(uint32_t parent_pid, uint8_t churn_round, uint32_t *out_pid)
{
    if ((churn_round & 1u) == 0u) {
        return kernel_ring3_spawn_fault_ud_probe(parent_pid, out_pid);
    }
    return kernel_ring3_spawn_fault_gp_probe(parent_pid, out_pid);
}

int
kernel_ring3_spawn_suite(uint32_t init_pid,
                         uint8_t ring3_thread_lifecycle_smoke_enabled,
                         uint8_t ring3_fault_churn_rounds)
{
    uint32_t ring3_smoke_pid = 0;
    uint32_t ring3_native_pid = 0;
    uint32_t ring3_threading_pid = 0;
    uint32_t ring3_fault_pid = 0;
    uint32_t ring3_fault_write_pid = 0;
    uint32_t ring3_fault_exec_pid = 0;
    uint32_t ring3_fault_ud_pid = 0;
    uint32_t ring3_fault_gp_pid = 0;
    uint32_t ring3_fault_de_pid = 0;
    uint32_t ring3_fault_db_pid = 0;
    uint32_t ring3_fault_bp_pid = 0;
    uint32_t ring3_fault_of_pid = 0;
    uint32_t ring3_fault_nm_pid = 0;
    uint32_t ring3_fault_ss_pid = 0;
    uint32_t ring3_fault_ac_pid = 0;

    if (kernel_ring3_spawn_smoke_process(init_pid, &ring3_smoke_pid) != 0) {
        klog_write("[kernel] ring3 smoke spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_native_probe(init_pid, &ring3_native_pid) != 0) {
        klog_write("[kernel] ring3 native spawn failed\n");
        return -1;
    }
    if (ring3_thread_lifecycle_smoke_enabled &&
        kernel_ring3_spawn_thread_lifecycle_probe(init_pid, &ring3_threading_pid) != 0) {
        klog_write("[kernel] ring3 threading spawn failed\n");
        return -1;
    }

    process_t *ring3_smoke_proc = process_get(ring3_smoke_pid);
    process_t *ring3_native_proc = process_get(ring3_native_pid);
    if (!ring3_smoke_proc || !ring3_native_proc) {
        klog_write("[test] ring3 shmem setup failed\n");
    } else {
        kernel_ring3_shmem_isolation_test(ring3_smoke_proc->context_id, ring3_native_proc->context_id);
    }

    if (kernel_ring3_spawn_fault_probe(init_pid, &ring3_fault_pid) != 0) {
        klog_write("[kernel] ring3 fault spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_write_probe(init_pid, &ring3_fault_write_pid) != 0) {
        klog_write("[kernel] ring3 fault write spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_exec_probe(init_pid, &ring3_fault_exec_pid) != 0) {
        klog_write("[kernel] ring3 fault exec spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_ud_probe(init_pid, &ring3_fault_ud_pid) != 0) {
        klog_write("[kernel] ring3 fault ud spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_gp_probe(init_pid, &ring3_fault_gp_pid) != 0) {
        klog_write("[kernel] ring3 fault gp spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_de_probe(init_pid, &ring3_fault_de_pid) != 0) {
        klog_write("[kernel] ring3 fault de spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_db_probe(init_pid, &ring3_fault_db_pid) != 0) {
        klog_write("[kernel] ring3 fault db spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_bp_probe(init_pid, &ring3_fault_bp_pid) != 0) {
        klog_write("[kernel] ring3 fault bp spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_of_probe(init_pid, &ring3_fault_of_pid) != 0) {
        klog_write("[kernel] ring3 fault of spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_nm_probe(init_pid, &ring3_fault_nm_pid) != 0) {
        klog_write("[kernel] ring3 fault nm spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_ss_probe(init_pid, &ring3_fault_ss_pid) != 0) {
        klog_write("[kernel] ring3 fault ss spawn failed\n");
        return -1;
    }
    if (kernel_ring3_spawn_fault_ac_probe(init_pid, &ring3_fault_ac_pid) != 0) {
        klog_write("[kernel] ring3 fault ac spawn failed\n");
        return -1;
    }

    ring3_fault_policy_probes_t fault_probes = {
        .fault_pid = ring3_fault_pid,
        .fault_write_pid = ring3_fault_write_pid,
        .fault_exec_pid = ring3_fault_exec_pid,
        .fault_ud_pid = ring3_fault_ud_pid,
        .fault_gp_pid = ring3_fault_gp_pid,
        .fault_de_pid = ring3_fault_de_pid,
        .fault_db_pid = ring3_fault_db_pid,
        .fault_bp_pid = ring3_fault_bp_pid,
        .fault_of_pid = ring3_fault_of_pid,
        .fault_nm_pid = ring3_fault_nm_pid,
        .fault_ss_pid = ring3_fault_ss_pid,
        .fault_ac_pid = ring3_fault_ac_pid
    };
    if (kernel_ring3_fault_policy_spawn(init_pid,
                                        &fault_probes,
                                        ring3_fault_churn_rounds,
                                        spawn_ring3_fault_churn_probe_process) != 0) {
        return -1;
    }
    return 0;
}
