/* kpanic.c - Unified kernel panic with cross-CPU NMI stop + full dump.
 * See kpanic.h for the flow. */

#include "kpanic.h"
#include "kallsyms.h"
#include "serial.h"
#include "paging.h"
#include "process.h"
#include "thread.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/lapic.h"

extern char __kernel_start[];
extern char __kernel_end[];

/* Register-frame layout as pushed by isr_nmi (PUSH_REGS then the CPU iret
 * frame). PUSH_REGS pushes rax,rbx,rcx,rdx,rbp,rsi,rdi,r8..r15 in that order,
 * so from the lowest saved address (rsp on entry to the C handler): */
enum {
    NMI_REG_RBP = 10, /* PUSH_REGS pushed rbp 5th => 5th from the top       */
    NMI_REG_RIP = 15, /* iret frame: rip                                    */
    NMI_REG_CS = 16,
    NMI_REG_RFLAGS = 17,
    NMI_REG_RSP = 18,
};

typedef struct {
    volatile uint32_t captured;
    uint64_t rip, rsp, rbp, rflags, cs;
    uint32_t pid, tid;
} panic_cpu_ctx_t;

static panic_cpu_ctx_t g_panic_ctx[WASMOS_MAX_CPUS];
static volatile uint32_t g_panicking; /* 0 until the first CPU wins the panic  */

/* Names an address, or says why it has no name.  Only kernel-range addresses are
 * looked up: kallsyms answers with the nearest preceding symbol whatever it is
 * handed, so a guest RIP would come back wearing the name of whichever kernel
 * symbol sorts below it -- a wrong answer that reads exactly like a right one. */
static void panic_print_symbol(uint64_t addr) {
    if (addr < paging_get_higher_half_base()) {
        serial_printf_unlocked(" (user/guest)");
        return;
    }
    const char* name = 0;
    uint64_t sym_addr = 0;
    if (kpanic_symbolize(addr, &name, &sym_addr) != 0 && name && *name) {
        serial_printf_unlocked(" (%s)", name);
    }
}

/* Conservative "could this plausibly be a kernel stack pointer?" screen for the
 * frame-pointer walk: a nested fault mid-panic would be fatal, so obviously bad
 * frame pointers are rejected before dereferencing.  Only alignment and the
 * higher-half range are checked, so a mapped-looking but bogus address still
 * gets through. */
static int panic_ptr_ok(uint64_t p) {
    uint64_t hh = paging_get_higher_half_base();
    if (p == 0 || (p & 0x7u) != 0u) {
        return 0; /* null or misaligned                            */
    }
    if (p < hh) {
        return 0; /* not in the higher-half kernel VA window        */
    }
    return 1;
}

/* Best-effort frame-pointer backtrace. Bounded depth; stops at the first
 * suspicious frame rather than risk a fault. */
static void panic_backtrace(uint64_t rbp) {
    for (int i = 0; i < 16; ++i) {
        if (!panic_ptr_ok(rbp)) {
            break;
        }
        const uint64_t* frame = ptr_cast(uint64_t, rbp);
        uint64_t next = frame[0];
        uint64_t ret = frame[1];
        serial_printf_unlocked("    [%d] ret=%016llx", i, (unsigned long long)ret);
        panic_print_symbol(ret);
        serial_printf_unlocked("\n");
        if (next <= rbp) {
            break; /* frame chain must climb toward the stack base   */
        }
        rbp = next;
    }
}

/* Records the faulting register state for the CALLING CPU so a later kpanic
 * prints the exception's own frame instead of kpanic's.  Meant to be called from
 * an exception entry path, before it decides to panic.
 *
 * The capture is published with a release store to a per-CPU slot, so it is the
 * last write and a reader that sees `captured` sees the whole frame.  It
 * overwrites any earlier capture for this CPU, so the most recent one wins.  A
 * CPU id at or above WASMOS_MAX_CPUS is ignored. */
void kpanic_capture_origin(uint64_t rip, uint64_t rsp, uint64_t rbp, uint64_t rflags, uint64_t cs) {
    uint32_t self = cpu_local()->cpu_id;
    if (self >= WASMOS_MAX_CPUS) {
        return;
    }

    panic_cpu_ctx_t* c = &g_panic_ctx[self];
    c->rip = rip;
    c->rsp = rsp;
    c->rbp = rbp;
    c->rflags = rflags;
    c->cs = cs;
    c->pid = cpu_local()->current_process ? cpu_local()->current_process->pid : 0u;
    c->tid = cpu_local()->current_thread ? cpu_local()->current_thread->tid : 0u;
    __atomic_store_n(&c->captured, 1u, __ATOMIC_RELEASE);
}

static const char* diag_thread_state_name(thread_state_t state) {
    switch (state) {
    case THREAD_STATE_UNUSED:
        return "unused";
    case THREAD_STATE_READY:
        return "ready";
    case THREAD_STATE_RUNNING:
        return "running";
    case THREAD_STATE_BLOCKED:
        return "blocked";
    case THREAD_STATE_ZOMBIE:
        return "zombie";
    case THREAD_STATE_NEW:
        return "new";
    default:
        return "?";
    }
}

static const char* diag_block_reason_name(thread_block_reason_t reason) {
    switch (reason) {
    case THREAD_BLOCK_NONE:
        return "-";
    case THREAD_BLOCK_IPC:
        return "ipc";
    case THREAD_BLOCK_WAIT_PROCESS:
        return "wait-proc";
    case THREAD_BLOCK_WAIT_THREAD:
        return "join";
    case THREAD_BLOCK_EVENT:
        return "event";
    default:
        return "?";
    }
}

/* Up to `frames` return addresses from a saved frame pointer, on one line.
 * Shares panic_ptr_ok's conservative screen: a wedged machine is exactly where a
 * bad frame pointer would turn a diagnostic into a second fault. */
static void diag_print_backtrace(uint64_t rbp, int frames) {
    for (int i = 0; i < frames; ++i) {
        if (!panic_ptr_ok(rbp)) {
            return;
        }
        const uint64_t* frame = ptr_cast(uint64_t, rbp);
        uint64_t next = frame[0];
        uint64_t ret = frame[1];
        serial_printf_unlocked(" [%d] %016llx", i, (unsigned long long)ret);
        panic_print_symbol(ret);
        if (next <= rbp) {
            return;
        }
        rbp = next;
    }
}

/* One line per live thread: what it is, what state it is in, and the three
 * facts that separate the ways a system can stop making progress.
 *
 *   rq=0 on a READY thread   -- runnable but on no run queue: a wake that
 *                               promoted the thread and never enqueued it, and
 *                               nothing will ever pick it up.
 *   wake=1 on a BLOCKED one  -- a waker deferred the enqueue to the blocking
 *                               thread's own completion path, which then did
 *                               not run it.
 *   blocked with a reason    -- ordinary waiting; a whole system of these and
 *                               no runnable thread is a deadlock between
 *                               processes, not a scheduler defect.
 *
 * Takes NO locks and loads nothing atomically, deliberately: it runs from the
 * NMI path on a machine that may be wedged holding any lock, where blocking to
 * read consistent state would mean printing nothing at all. A torn field is an
 * acceptable price for a snapshot that always arrives; treat a single
 * implausible line as a race rather than evidence.
 *
 * Everything goes through the UNLOCKED serial writer, for the same reason. */
void diag_dump_threads(const char* reason) {
    serial_printf_unlocked("[diag] thread table (%s)\n", reason ? reason : "-");
    /* Which thread each CPU believes it is running, so a thread that says
     * RUNNING can be checked against the CPUs rather than believed.  A thread in
     * that state which no CPU is executing is orphaned: it is on no run queue
     * either (enqueue requires READY), so nothing will ever dispatch it again --
     * the condition process.c's PROCESS_RUN_BLOCKED handler exists to prevent
     * for legacy blockers, and a boot hang under SMP the last time it occurred. */
    uint32_t current_tids[WASMOS_MAX_CPUS];
    uint32_t cpu_count = g_cpu_count > WASMOS_MAX_CPUS ? WASMOS_MAX_CPUS : g_cpu_count;
    for (uint32_t c = 0; c < cpu_count; ++c) {
        cpu_local_t* cpu = &g_cpus[c];
        thread_t* cur = cpu->current_thread;
        thread_t* idle = cpu->idle_thread;
        current_tids[c] = cur ? cur->tid : 0u;
        serial_printf_unlocked(
            "[diag] cpu=%u cur_tid=%u cur_pid=%u idle_tid=%u in_sched=%u in_ctxsw=%u "
            "irq_depth=%u preempt=%u dispatches=%u\n",
            (unsigned)c,
            (unsigned)current_tids[c],
            (unsigned)(cpu->current_process ? cpu->current_process->pid : 0u),
            (unsigned)(idle ? idle->tid : 0u),
            (unsigned)cpu->in_scheduler,
            (unsigned)cpu->in_context_switch,
            (unsigned)cpu->irq_disable_depth,
            (unsigned)cpu->preempt_disable_count,
            (unsigned)cpu->dispatch_count);
    }
    uint32_t live = 0;
    uint32_t ready = 0;
    uint32_t blocked = 0;
    uint32_t stranded = 0;
    uint32_t orphaned = 0;
    for (uint32_t i = 0; i < THREAD_MAX_COUNT; ++i) {
        thread_t* t = thread_table_at(i);
        if (!t || t->tid == 0 || t->state == THREAD_STATE_UNUSED) {
            continue;
        }
        live++;
        process_t* proc = process_get(t->owner_pid);
        /* An idle thread is READY and never queued by design: it lives in the
         * per-CPU fallback path, so counting it as stranded would report the
         * anomaly on every healthy dump. */
        uint8_t is_idle = (proc && proc->is_idle) ? 1u : 0u;
        uint8_t anomaly = 0;
        if (t->state == THREAD_STATE_READY) {
            ready++;
            if (!t->on_rq && !is_idle) {
                stranded++;
                anomaly = 1;
            }
        } else if (t->state == THREAD_STATE_RUNNING) {
            uint8_t current_somewhere = 0;
            for (uint32_t c = 0; c < cpu_count; ++c) {
                if (current_tids[c] == t->tid) {
                    current_somewhere = 1;
                    break;
                }
            }
            if (!current_somewhere) {
                orphaned++;
                anomaly = 1;
            }
        } else if (t->state == THREAD_STATE_BLOCKED) {
            blocked++;
            if (t->wake_pending) {
                anomaly = 1; /* a wake was deferred to a completion path that never ran */
            }
        }
        serial_printf_unlocked(
            "[diag]%s tid=%u pid=%u %s st=%s rsn=%s rq=%u wake=%u btrans=%u ev=%u cpu=%u "
            "ticks=%llu\n",
            anomaly ? "!" : "",
            (unsigned)t->tid,
            (unsigned)t->owner_pid,
            (proc && proc->name) ? proc->name : "?",
            diag_thread_state_name(t->state),
            diag_block_reason_name(t->block_reason),
            (unsigned)t->on_rq,
            (unsigned)t->wake_pending,
            (unsigned)t->blocking_transition,
            (unsigned)(t->wait_event ? 1u : 0u),
            (unsigned)t->last_cpu,
            (unsigned long long)t->ticks_total);
        /* Where the thread stopped. ctx is the context the scheduler saved when
         * it last switched away, so for a BLOCKED thread this is the blocking
         * call site and the frames above it -- the difference between "26
         * threads are waiting" and "fs-manager is waiting inside a query it
         * made to device-manager". It is stale for a RUNNING thread, which is
         * executing past it on some CPU; the state field says which. */
        serial_printf_unlocked("[diag]   rip=%016llx", (unsigned long long)t->ctx.rip);
        panic_print_symbol(t->ctx.rip);
        if (t->state == THREAD_STATE_BLOCKED) {
            diag_print_backtrace(t->ctx.rbp, 4);
        }
        serial_printf_unlocked("\n");
    }
    /* A line marked `[diag]!` is one of the two scheduler anomalies above; a
     * dump with none of them and no runnable thread is a deadlock between
     * processes rather than a lost wake. */
    serial_printf_unlocked(
        "[diag] live=%u ready=%u blocked=%u stranded(ready,no-rq)=%u orphaned(running,no-cpu)=%u\n",
        (unsigned)live,
        (unsigned)ready,
        (unsigned)blocked,
        (unsigned)stranded,
        (unsigned)orphaned);
}

/* NMI vector, with two entirely different behaviours depending on whether a
 * panic is in progress.
 *
 * Outside a panic the NMI was not ours: it is logged through the UNLOCKED serial
 * writer (an NMI can interrupt a CPU holding the serial lock) and the handler
 * RETURNS, resuming the interrupted code.
 *
 * During a panic the NMI is the panicking CPU's stop signal.  This CPU publishes
 * its register frame into its per-CPU panic slot so the dump can include it, and
 * then HALTS FOREVER with interrupts masked — this path does not return.
 *
 * regs points at the interrupt frame saved by the NMI stub, indexed by the
 * NMI_REG_* constants, and is borrowed for the call. */
void x86_nmi_handler(uint64_t* regs) {
    if (!__atomic_load_n(&g_panicking, __ATOMIC_ACQUIRE)) {
        /* Not a panic stop — an NMI the kernel did not initiate, which is how a
         * diagnosis is requested from outside: the test framework injects one
         * over the QEMU monitor when a command stalls. The thread table is what
         * that asks for, since the wedge it chases leaves every CPU idle and the
         * question is which threads are waiting and on what.
         *
         * Only the first CPU to arrive dumps. QEMU delivers the NMI to every
         * vCPU, and four interleaved copies of one table are worse than one. */
        serial_printf_unlocked("[nmi] unexpected NMI cpu=%u rip=%016llx\n",
                               (unsigned)cpu_local()->cpu_id,
                               (unsigned long long)regs[NMI_REG_RIP]);
        static volatile uint32_t nmi_diag_busy;
        if (__atomic_exchange_n(&nmi_diag_busy, 1u, __ATOMIC_ACQ_REL) == 0u) {
            diag_dump_threads("unexpected NMI");
            __atomic_store_n(&nmi_diag_busy, 0u, __ATOMIC_RELEASE);
        }
        return;
    }

    uint32_t id = cpu_local()->cpu_id;
    if (id < WASMOS_MAX_CPUS) {
        panic_cpu_ctx_t* c = &g_panic_ctx[id];
        c->rip = regs[NMI_REG_RIP];
        c->cs = regs[NMI_REG_CS];
        c->rflags = regs[NMI_REG_RFLAGS];
        c->rsp = regs[NMI_REG_RSP];
        c->rbp = regs[NMI_REG_RBP];
        c->pid = cpu_local()->current_process ? cpu_local()->current_process->pid : 0u;
        c->tid = cpu_local()->current_thread ? cpu_local()->current_thread->tid : 0u;
        __atomic_store_n(&c->captured, 1u, __ATOMIC_RELEASE);
    }
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* Stops the machine and dumps every CPU's state.  Does not return under any
 * circumstances.
 *
 * reason is a short tag printed verbatim; NULL prints "(none)".  a and b are two
 * free-form 64-bit values printed in hex, by convention the identifiers that
 * make the failure diagnosable.
 *
 * Only the FIRST CPU to arrive produces the dump; a second panicking CPU halts
 * immediately so the output is not interleaved.  The winner NMIs every other CPU
 * to make it snapshot itself, waits a bounded spin for those snapshots, then
 * prints the reason and, per CPU, its frame and a backtrace.  Its own frame is
 * captured here only if kpanic_capture_origin has not already filled the slot,
 * so an exception's frame takes precedence over kpanic's own call site.
 *
 * Everything is written with the UNLOCKED serial writers: the serial lock may be
 * held by an interrupted CPU that will never release it.  A CPU that never
 * answers the NMI is reported as uncaptured rather than waited for
 * indefinitely. */
__attribute__((noreturn)) void kpanic(const char* reason, uint64_t a, uint64_t b) {
    __asm__ volatile("cli");

    /* First CPU to panic wins; any later panicker (including an NMI'd CPU that
     * then also called kpanic) just halts so the dump isn't interleaved. */
    if (__atomic_exchange_n(&g_panicking, 1u, __ATOMIC_ACQ_REL) != 0u) {
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    uint32_t self = cpu_local()->cpu_id;

    /* Capture this CPU's own context directly. */
    if (self < WASMOS_MAX_CPUS) {
        panic_cpu_ctx_t* c = &g_panic_ctx[self];
        if (!__atomic_load_n(&c->captured, __ATOMIC_ACQUIRE)) {
            uint64_t rbp = 0, rsp = 0, rflags = 0;
            __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
            __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
            __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
            c->rip = addr_cast(uint64_t, __builtin_return_address(0));
            c->rbp = rbp;
            c->rsp = rsp;
            c->rflags = rflags;
            c->cs = 0;
            c->pid = cpu_local()->current_process ? cpu_local()->current_process->pid : 0u;
            c->tid = cpu_local()->current_thread ? cpu_local()->current_thread->tid : 0u;
            __atomic_store_n(&c->captured, 1u, __ATOMIC_RELEASE);
        }
    }

#if WASMOS_SMP
    /* Stop every other CPU. NMI ignores IF, so a CPU spinning under cli is still
     * caught. */
    lapic_send_nmi_allbutself();
#endif

    /* Wait (bounded) for the others to snapshot themselves. */
    for (volatile uint64_t spin = 0; spin < 200000000ULL; ++spin) {
        int all = 1;
        for (uint32_t i = 0; i < g_cpu_count && i < WASMOS_MAX_CPUS; ++i) {
            if (i == self) {
                continue;
            }
            if (g_cpus[i].started && !g_panic_ctx[i].captured) {
                all = 0;
                break;
            }
        }
        if (all) {
            break;
        }
        __asm__ volatile("pause");
    }

    serial_printf_unlocked("\n================= KERNEL PANIC =================\n");
    serial_printf_unlocked("reason : %s\n", reason ? reason : "(none)");
    serial_printf_unlocked("a=%016llx b=%016llx\n", (unsigned long long)a, (unsigned long long)b);
    serial_printf_unlocked("cpus=%u  panicking_cpu=%u\n", (unsigned)g_cpu_count, (unsigned)self);

    for (uint32_t i = 0; i < g_cpu_count && i < WASMOS_MAX_CPUS; ++i) {
        panic_cpu_ctx_t* c = &g_panic_ctx[i];
        serial_printf_unlocked("--- CPU %u captured=%u pid=%u tid=%u ---\n",
                               (unsigned)i,
                               (unsigned)c->captured,
                               (unsigned)c->pid,
                               (unsigned)c->tid);
        if (!c->captured) {
            serial_printf_unlocked("    <no NMI capture (offline or stuck with NMIs masked)>\n");
            continue;
        }
        serial_printf_unlocked("    rip=%016llx rsp=%016llx rbp=%016llx rflags=%016llx\n",
                               (unsigned long long)c->rip,
                               (unsigned long long)c->rsp,
                               (unsigned long long)c->rbp,
                               (unsigned long long)c->rflags);
        serial_printf_unlocked("    backtrace:\n");
        panic_backtrace(c->rbp);
    }
    serial_printf_unlocked("=============== END KERNEL PANIC ===============\n");

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
