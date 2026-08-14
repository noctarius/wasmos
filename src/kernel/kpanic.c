/* kpanic.c - Unified kernel panic with cross-CPU NMI stop + full dump.
 * See kpanic.h for the flow. */

#include "kpanic.h"
#include "kallsyms.h"
#include "serial.h"
#include "paging.h"
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

static void panic_print_symbol(uint64_t addr) {
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
        /* Not a panic stop — an NMI the kernel did not initiate. Log and resume. */
        serial_printf_unlocked("[nmi] unexpected NMI cpu=%u rip=%016llx\n",
                               (unsigned)cpu_local()->cpu_id,
                               (unsigned long long)regs[NMI_REG_RIP]);
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
        serial_printf_unlocked("--- CPU %u captured=%u pid=%u tid=%u ---\n", (unsigned)i,
                               (unsigned)c->captured, (unsigned)c->pid, (unsigned)c->tid);
        if (!c->captured) {
            serial_printf_unlocked("    <no NMI capture (offline or stuck with NMIs masked)>\n");
            continue;
        }
        serial_printf_unlocked("    rip=%016llx rsp=%016llx rbp=%016llx rflags=%016llx\n",
                               (unsigned long long)c->rip, (unsigned long long)c->rsp,
                               (unsigned long long)c->rbp, (unsigned long long)c->rflags);
        serial_printf_unlocked("    backtrace:\n");
        panic_backtrace(c->rbp);
    }
    serial_printf_unlocked("=============== END KERNEL PANIC ===============\n");

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
