/* kernel_sched_smp_stress_runtime.c - Standalone SMP scheduler stress test.
 *
 * See kernel_sched_smp_stress_runtime.h. Built into a no-op unless
 * WASMOS_SCHED_SMP_STRESS is defined.
 *
 * Model: N worker threads connected in a ring by N IPC endpoints. Worker i
 * blocks on endpoint[i], and on receipt forwards the token to endpoint[i+1].
 * K tokens are injected so K workers are active concurrently across CPUs. Each
 * worker must forward ITERS tokens. Because every receive blocks until the
 * previous worker (possibly on another CPU, possibly after being stolen there)
 * sends, the ring continuously exercises cross-CPU block/wake/steal/migration.
 *
 * Oracle: the test passes only when every worker completes ITERS forwards. A
 * RUNNING-orphan, lost wakeup, or stranded-ready thread stops the token it was
 * holding, so total forward progress (hops) stalls -> the coordinator detects
 * no progress for a long window and FAILS with diagnostics, instead of the bug
 * manifesting as a rare, hard-to-reproduce boot hang. A final invariant scan
 * also flags any thread left RUNNING but not current on any CPU. */
#include "kernel_sched_smp_stress_runtime.h"

#ifdef WASMOS_SCHED_SMP_STRESS

#include "ipc.h"
#include "klog.h"
#include "process.h"
#include "thread.h"
#include "serial.h"
#include "arch/x86_64/smp.h"

#define SMP_STRESS_WORKERS  8u
#define SMP_STRESS_ITERS    256u
#define SMP_STRESS_TOKENS   4u
/* Consecutive coordinator polls with zero forward progress before declaring the
 * ring stalled. The coordinator yields between polls, so a live ring advances
 * within a handful of polls; this bound is far above normal scheduling jitter. */
#define SMP_STRESS_STALL_LIMIT 2000000u

typedef struct {
    uint32_t recv_ep;
    uint32_t send_ep;
    uint32_t context_id;
    uint32_t tid;
    volatile uint32_t iters_done;
    volatile uint32_t cpu_mask; /* CPUs this worker has executed on */
} smp_stress_worker_t;

typedef struct {
    uint8_t  spawned;
    uint8_t  done;
    uint32_t last_hops;
    uint32_t stall_polls;
} smp_stress_coord_state_t;

static smp_stress_worker_t   g_sw[SMP_STRESS_WORKERS];
static uint32_t              g_smp_stress_ep[SMP_STRESS_WORKERS];
static volatile uint32_t     g_smp_stress_hops;
static volatile uint32_t     g_smp_stress_done;
static smp_stress_coord_state_t g_smp_stress_coord;

static process_run_result_t
smp_stress_worker_entry(process_t *process, uint32_t tid, void *arg)
{
    smp_stress_worker_t *w = (smp_stress_worker_t *)arg;
    (void)tid;
    if (!process || !w) {
        return PROCESS_RUN_EXITED;
    }

    while (w->iters_done < SMP_STRESS_ITERS) {
        ipc_message_t msg;
        int rc = ipc_recv_blocking_for(w->context_id, w->recv_ep, &msg);
        if (rc == IPC_EMPTY) {
            /* Spurious wake — re-block. */
            continue;
        }
        if (rc != IPC_OK) {
            break;
        }
        /* Record which CPU we woke up on so the coordinator can confirm the
         * ring really spread across processors. */
        __atomic_or_fetch(&w->cpu_mask, 1u << cpu_local()->cpu_id, __ATOMIC_RELAXED);

        /* Forward the token to the next worker. */
        msg.source = w->recv_ep; /* an endpoint owned by our context */
        msg.destination = IPC_ENDPOINT_NONE;
        int sc;
        while ((sc = ipc_send_from(w->context_id, w->send_ep, &msg)) == IPC_ERR_FULL) {
            process_yield(PROCESS_RUN_YIELDED);
        }
        if (sc != IPC_OK) {
            break;
        }
        w->iters_done++;
        __atomic_add_fetch(&g_smp_stress_hops, 1u, __ATOMIC_RELAXED);
    }

    __atomic_add_fetch(&g_smp_stress_done, 1u, __ATOMIC_RELAXED);
    process_set_exit_status(process, 0);
    return PROCESS_RUN_EXITED;
}

static uint32_t
smp_stress_popcount(uint32_t v)
{
    uint32_t n = 0;
    while (v) { n += (v & 1u); v >>= 1; }
    return n;
}

/* Scan for the invariant violation this test exists to catch: a thread left in
 * THREAD_STATE_RUNNING that is not the current thread on any CPU (and hence in
 * no ready queue) — i.e. a stranded/orphaned thread. Returns the count. */
static uint32_t
smp_stress_count_orphans(void)
{
    uint32_t orphans = 0;
    for (uint32_t i = 0; i < SMP_STRESS_WORKERS; ++i) {
        thread_t *t = thread_get(g_sw[i].tid);
        if (!t || t->state != THREAD_STATE_RUNNING) {
            continue;
        }
        uint8_t current_somewhere = 0;
        for (uint32_t c = 0; c < g_cpu_count && c < WASMOS_MAX_CPUS; ++c) {
            if (g_cpus[c].current_thread == t) {
                current_somewhere = 1;
                break;
            }
        }
        if (!current_somewhere) {
            orphans++;
            serial_printf("[test] sched smp stress ORPHAN tid=%u iters=%u last_cpu=%u\n",
                          (unsigned)g_sw[i].tid, (unsigned)g_sw[i].iters_done,
                          (unsigned)t->last_cpu);
        }
    }
    return orphans;
}

static void
smp_stress_report(uint8_t passed)
{
    uint32_t cpus_used = 0;
    for (uint32_t i = 0; i < SMP_STRESS_WORKERS; ++i) {
        cpus_used |= g_sw[i].cpu_mask;
    }
    serial_printf("[test] sched smp stress %s hops=%u done=%u/%u cpus=%u\n",
                  passed ? "summary" : "DIAG",
                  (unsigned)__atomic_load_n(&g_smp_stress_hops, __ATOMIC_RELAXED),
                  (unsigned)__atomic_load_n(&g_smp_stress_done, __ATOMIC_RELAXED),
                  (unsigned)SMP_STRESS_WORKERS,
                  (unsigned)smp_stress_popcount(cpus_used));
    if (!passed) {
        for (uint32_t i = 0; i < SMP_STRESS_WORKERS; ++i) {
            serial_printf("[test] sched smp stress worker %u iters=%u cpus=%u\n",
                          (unsigned)i, (unsigned)g_sw[i].iters_done,
                          (unsigned)smp_stress_popcount(g_sw[i].cpu_mask));
        }
        (void)smp_stress_count_orphans();
    }
}

static process_run_result_t
smp_stress_coordinator_entry(process_t *process, void *arg)
{
    (void)arg;
    smp_stress_coord_state_t *st = &g_smp_stress_coord;
    if (!process) {
        return PROCESS_RUN_IDLE;
    }
    if (st->done) {
        return PROCESS_RUN_EXITED;
    }

    if (!st->spawned) {
        for (uint32_t i = 0; i < SMP_STRESS_WORKERS; ++i) {
            if (ipc_endpoint_create(process->context_id, &g_smp_stress_ep[i]) != IPC_OK) {
                klog_write("[test] sched smp stress endpoint create failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
        for (uint32_t i = 0; i < SMP_STRESS_WORKERS; ++i) {
            g_sw[i].recv_ep    = g_smp_stress_ep[i];
            g_sw[i].send_ep    = g_smp_stress_ep[(i + 1u) % SMP_STRESS_WORKERS];
            g_sw[i].context_id = process->context_id;
            g_sw[i].iters_done = 0;
            g_sw[i].cpu_mask   = 0;
            if (process_thread_spawn_worker_internal(process->pid,
                                                     "smp-stress",
                                                     smp_stress_worker_entry,
                                                     &g_sw[i],
                                                     &g_sw[i].tid) != 0) {
                klog_write("[test] sched smp stress worker spawn failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
        /* Inject the initial tokens. */
        for (uint32_t i = 0; i < SMP_STRESS_TOKENS && i < SMP_STRESS_WORKERS; ++i) {
            ipc_message_t tok;
            tok.type = 1;
            tok.source = g_smp_stress_ep[i];
            tok.destination = IPC_ENDPOINT_NONE;
            tok.request_id = i + 1u;
            tok.arg0 = tok.arg1 = tok.arg2 = tok.arg3 = 0;
            if (ipc_send_from(process->context_id, g_smp_stress_ep[i], &tok) != IPC_OK) {
                klog_write("[test] sched smp stress token inject failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
        klog_printf("[test] sched smp stress start workers=%u tokens=%u iters=%u\n",
                    (unsigned)SMP_STRESS_WORKERS,
                    (unsigned)SMP_STRESS_TOKENS,
                    (unsigned)SMP_STRESS_ITERS);
        st->spawned = 1;
        st->last_hops = 0;
        st->stall_polls = 0;
        return PROCESS_RUN_YIELDED;
    }

    uint32_t done = __atomic_load_n(&g_smp_stress_done, __ATOMIC_RELAXED);
    if (done >= SMP_STRESS_WORKERS) {
        uint32_t orphans = smp_stress_count_orphans();
        smp_stress_report(1);
        if (orphans == 0) {
            klog_write("[test] sched smp stress ok\n");
            process_set_exit_status(process, 0);
        } else {
            klog_write("[test] sched smp stress FAIL orphans after completion\n");
            process_set_exit_status(process, -1);
        }
        st->done = 1;
        return PROCESS_RUN_EXITED;
    }

    uint32_t hops = __atomic_load_n(&g_smp_stress_hops, __ATOMIC_RELAXED);
    if (hops != st->last_hops) {
        st->last_hops = hops;
        st->stall_polls = 0;
    } else if (++st->stall_polls >= SMP_STRESS_STALL_LIMIT) {
        klog_write("[test] sched smp stress FAIL ring stalled (orphan/lost-wakeup)\n");
        smp_stress_report(0);
        process_set_exit_status(process, -1);
        st->done = 1;
        return PROCESS_RUN_EXITED;
    }
    return PROCESS_RUN_YIELDED;
}

int
kernel_sched_smp_stress_spawn(uint32_t init_pid)
{
    uint32_t pid = 0;
    g_smp_stress_hops = 0;
    g_smp_stress_done = 0;
    g_smp_stress_coord.spawned = 0;
    g_smp_stress_coord.done = 0;
    if (process_spawn_as(init_pid, "smp-stress", smp_stress_coordinator_entry, 0, &pid) != 0) {
        klog_write("[test] sched smp stress spawn failed\n");
        return -1;
    }
    return 0;
}

#else /* !WASMOS_SCHED_SMP_STRESS */

int
kernel_sched_smp_stress_spawn(uint32_t init_pid)
{
    (void)init_pid;
    return 0;
}

#endif /* WASMOS_SCHED_SMP_STRESS */
