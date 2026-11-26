/* cpu_x86_64.h - x86_64 CPU state: GDT, IDT, TSS, and per-CPU data declarations. */
#ifndef WASMOS_ARCH_X86_64_CPU_X86_64_H
#define WASMOS_ARCH_X86_64_CPU_X86_64_H

#include <stdint.h>

/* Per-CPU GDT length: null, kernel code, kernel data, user code, user data, then two
 * slots for the 16-byte 64-bit TSS descriptor, which occupies two 8-byte entries (Intel
 * SDM, "System Descriptor Types" / "TSS Descriptor in 64-bit mode"). */
#define GDT_ENTRY_COUNT 7
/* Bytes per interrupt stack: both the TSS's rsp0 (ring-3 -> ring-0 transitions) and the
 * IST1 stack used by the timer gate.  Each stack's lowest 8 bytes hold a canary that
 * cpu_isr.S checks on entry, so an overflow is detected instead of corrupting whatever
 * precedes the stack. */
#define CPU_IST_STACK_SIZE 16384u

/* 64-bit Task State Segment, laid out exactly as the hardware reads it (Intel SDM, "Task
 * Management in 64-bit Mode" / "64-Bit TSS Format"): it is not a task-switch structure
 * here, only the place the CPU finds a stack pointer for a privilege or IST transition.
 * rsp0 is the stack loaded on a ring-3 -> ring-0 transition; rsp1/rsp2 are unused because
 * rings 1 and 2 are not.  ist1..ist7 are the interrupt-stack-table entries an IDT gate can
 * select by index; only IST1 is populated.  All of these are kernel virtual addresses of
 * the stack TOP, since the stack grows down.  `iopb` is the offset to the I/O permission
 * bitmap and is set to sizeof(tss_t), i.e. past the end of the segment, which denies all
 * port access from ring 3.  The reserved fields must stay zero. */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;
} tss_t;

/* cpu_init, cpu_set_kernel_stack and cpu_relocate_tables_high (cpu.h) are
 * implemented in cpu_x86_64.c. */

/* int 0x80 is the WASMOS syscall vector (the only DPL-3 IDT gate). */
#define X86_VECTOR_SYSCALL 0x80u

/* Handle a CPU exception raised in ring-3. `frame` is the CPU's interrupt frame
 * (rip at [1], cs at [2]). There is no signal delivery: a claimed fault
 * terminates the process with exit status -11 and never returns to the faulting
 * instruction. Returns 0 when the process was terminated, -1 when the fault is
 * declined -- no current process, cs says the fault came from ring 0, or the
 * vector is not one of the classified user vectors -- leaving the caller to
 * escalate to a kernel panic. Called from the cpu_isr.S exception stubs. */
int x86_user_exception_handler(uint64_t vector, const uint64_t* frame);

/* Handle a page fault; may satisfy it by demand paging, by retrying a stale TLB
 * entry in the shared linmem window, or by killing the faulting process.
 * Returns 0 when the fault was resolved or absorbed, -1 when it was not and the
 * caller must panic. Called from the cpu_isr.S #PF stub. */
int x86_page_fault_handler(uint64_t error_code, const uint64_t* frame);

/* The x86_64 half of the debug.exception_test boot option
 * (kernel_boot_exception_test): raise `vector` in ring 0 on a dedicated stack,
 * with rbp zero, after logging the rip and rsp the exception dump must report.
 * Vector 1 is a real single-step #DB, from setting TF; any other supported
 * vector is raised with `int`, whose frame matches a hardware exception of a
 * vector that pushes no error code. Returns only for an unsupported vector,
 * having logged that; otherwise the kernel panics. */
void x86_exception_test_raise(uint32_t vector);

/* C side of the NMI vector (isr_nmi in cpu_isr.S): decodes the pushed register
 * frame (PUSH_REGS order, then the CPU iret frame) and hands it to kpanic_nmi. */
void x86_nmi_handler(uint64_t* regs);

#if WASMOS_SMP
struct cpu_local;
/* Initialise the AP cpu_local slot: copy GDT template, zero TSS, set stack
 * tops, encode TSS into GDT.  Called from the BSP before sending SIPI. */
void x86_cpu_prepare_ap(struct cpu_local* cpu, uint64_t ist1_top, uint64_t rsp0_top);
/* Load the per-CPU GDT, IDT, TSS, and GS base on the calling AP.
 * Must be called from the AP itself after it reaches 64-bit long mode. */
void x86_ap_cpu_init(uint32_t cpu_id);
#endif

#endif
