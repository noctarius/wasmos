#include "cpu.h"
#include "serial.h"
#include "process.h"
#include "memory_service.h"
#include "irq.h"
#include <stdint.h>

#define GDT_ENTRY_COUNT 5
#define IDT_ENTRY_COUNT 256
#define EXCEPTION_COUNT 32

#define KERNEL_CS_SELECTOR 0x08
#define KERNEL_DS_SELECTOR 0x10
#define KERNEL_TSS_SELECTOR 0x18
#define IRQ0_IST_INDEX 1
#define IRQ0_IST_STACK_SIZE 16384u

#define IDT_TYPE_INTERRUPT_GATE 0x8E

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} descriptor_ptr_t;

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry_t;

extern void *x86_exception_stub_table[];
extern void *x86_irq_stub_table[];
extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

static uint64_t g_gdt[GDT_ENTRY_COUNT] = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL,
    0x00AF92000000FFFFULL,
    0x0000000000000000ULL,
    0x0000000000000000ULL,
};
static idt_entry_t g_idt[IDT_ENTRY_COUNT];
uint8_t g_irq0_ist_stack[IRQ0_IST_STACK_SIZE] __attribute__((aligned(16)));
uint64_t g_irq0_ist_canary = 0xCAFEBABEDEADC0DEULL;

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

static tss_t g_tss;


static void
serial_write_hexbyte_unlocked(uint8_t value)
{
    char buf[3];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = hex[(value >> 4) & 0xF];
    buf[1] = hex[value & 0xF];
    buf[2] = '\0';
    serial_write_unlocked(buf);
}

static void
serial_dump_bytes_unlocked(const char *label, const uint8_t *ptr, uint32_t count)
{
    if (!ptr || count == 0) {
        return;
    }
    if (label) {
        serial_write_unlocked(label);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (i == 0) {
            serial_write_unlocked(" ");
        } else {
            serial_write_unlocked(" ");
        }
        serial_write_hexbyte_unlocked(ptr[i]);
    }
    serial_write_unlocked("\n");
}

static void
gdt_install(void)
{
    descriptor_ptr_t gdtr;
    gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    gdtr.base = (uint64_t)(uintptr_t)&g_gdt[0];

    __asm__ volatile(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        :
        : "m"(gdtr)
        : "rax", "memory");

    uint16_t tss_selector = KERNEL_TSS_SELECTOR;
    __asm__ volatile("ltr %0" : : "r"(tss_selector) : "memory");
}

static void
idt_set_gate(uint8_t vector, uintptr_t handler, uint8_t type_attr)
{
    idt_entry_t *entry = &g_idt[vector];
    entry->offset_low = (uint16_t)(handler & 0xFFFFU);
    entry->selector = KERNEL_CS_SELECTOR;
    entry->ist = 0;
    entry->type_attr = type_attr;
    entry->offset_mid = (uint16_t)((handler >> 16) & 0xFFFFU);
    entry->offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFU);
    entry->zero = 0;
}

static void
idt_set_gate_ist(uint8_t vector, uintptr_t handler, uint8_t type_attr, uint8_t ist)
{
    idt_entry_t *entry = &g_idt[vector];
    entry->offset_low = (uint16_t)(handler & 0xFFFFU);
    entry->selector = KERNEL_CS_SELECTOR;
    entry->ist = ist;
    entry->type_attr = type_attr;
    entry->offset_mid = (uint16_t)((handler >> 16) & 0xFFFFU);
    entry->offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFU);
    entry->zero = 0;
}

static void
gdt_set_tss(void)
{
    uint64_t base = (uint64_t)(uintptr_t)&g_tss;
    uint32_t limit = (uint32_t)(sizeof(g_tss) - 1u);
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFFu);
    low |= (uint64_t)(base & 0xFFFFFFu) << 16;
    low |= (uint64_t)0x89u << 40;
    low |= (uint64_t)((limit >> 16) & 0xFu) << 48;
    low |= (uint64_t)((base >> 24) & 0xFFu) << 56;
    uint64_t high = (uint64_t)(base >> 32);
    g_gdt[3] = low;
    g_gdt[4] = high;
}

static void
tss_init(void)
{
    for (uint32_t i = 0; i < sizeof(g_tss); ++i) {
        ((uint8_t *)&g_tss)[i] = 0;
    }
    for (uint32_t i = 0; i < IRQ0_IST_STACK_SIZE; ++i) {
        g_irq0_ist_stack[i] = 0xCC;
    }
    *(uint64_t *)(uintptr_t)g_irq0_ist_stack = g_irq0_ist_canary;
    uint64_t ist1_top = (uint64_t)(uintptr_t)(g_irq0_ist_stack + IRQ0_IST_STACK_SIZE);
    g_tss.rsp0 = ist1_top;
    g_tss.ist1 = ist1_top;
    g_tss.iopb = (uint16_t)sizeof(g_tss);
    gdt_set_tss();
}

static void
idt_install(void)
{
    for (uint32_t i = 0; i < IDT_ENTRY_COUNT; ++i) {
        g_idt[i].offset_low = 0;
        g_idt[i].selector = 0;
        g_idt[i].ist = 0;
        g_idt[i].type_attr = 0;
        g_idt[i].offset_mid = 0;
        g_idt[i].offset_high = 0;
        g_idt[i].zero = 0;
    }

    for (uint32_t vec = 0; vec < EXCEPTION_COUNT; ++vec) {
        uintptr_t handler = (uintptr_t)x86_exception_stub_table[vec];
        idt_set_gate((uint8_t)vec, handler, IDT_TYPE_INTERRUPT_GATE);
    }

    descriptor_ptr_t idtr;
    idtr.limit = (uint16_t)(sizeof(g_idt) - 1);
    idtr.base = (uint64_t)(uintptr_t)&g_idt[0];
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

__attribute__((noreturn)) void
x86_exception_panic(uint64_t vector)
{
    serial_write("[cpu] exception vector=");
    serial_write_hex64(vector);

    if (vector == 14) {
        uint64_t cr2 = 0;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        serial_write("[cpu] page fault cr2=");
        serial_write_hex64(cr2);
    }

    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

__attribute__((noreturn)) void
x86_exception_panic_frame(uint64_t vector, const uint64_t *frame)
{
    uint64_t err = 0;
    uint64_t rip = 0;
    uint64_t cs = 0;
    uint64_t rflags = 0;
    uint32_t pid = process_current_pid();
    process_t *proc = process_get(pid);
    const char *name = proc && proc->name ? proc->name : 0;
    uint64_t stack_base = proc ? (uint64_t)proc->stack_base : 0;
    uint64_t stack_top = proc ? (uint64_t)proc->stack_top : 0;
    uint64_t kernel_start = (uint64_t)(uintptr_t)&__kernel_start;
    uint64_t kernel_end = (uint64_t)(uintptr_t)&__kernel_end;

    uint64_t cr2 = 0;
    if (vector == 14) {
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    }

    if (frame) {
        err = frame[0];
        rip = frame[1];
        cs = frame[2];
        rflags = frame[3];
    }

    serial_write_unlocked("[cpu] exception vector=");
    serial_write_hex64_unlocked(vector);
    serial_write_unlocked("[cpu] err=");
    serial_write_hex64_unlocked(err);
    serial_write_unlocked("[cpu] rip=");
    serial_write_hex64_unlocked(rip);
    serial_write_unlocked("[cpu] cs=");
    serial_write_hex64_unlocked(cs);
    serial_write_unlocked("[cpu] rflags=");
    serial_write_hex64_unlocked(rflags);
    if (vector == 14) {
        serial_write_unlocked("[cpu] cr2=");
        serial_write_hex64_unlocked(cr2);
    }
    serial_write_unlocked("[cpu] frame=");
    serial_write_hex64_unlocked((uint64_t)(uintptr_t)frame);
    serial_write_unlocked("[cpu] pid=");
    serial_write_hex64_unlocked(pid);
    serial_write_unlocked("[cpu] name=");
    if (name) {
        serial_write_unlocked(name);
    } else {
        serial_write_unlocked("(null)");
    }
    serial_write_unlocked("\n");
    serial_write_unlocked("[cpu] stack base=");
    serial_write_hex64_unlocked(stack_base);
    serial_write_unlocked("[cpu] stack top=");
    serial_write_hex64_unlocked(stack_top);
    if (rip >= kernel_start && (rip + 16) <= kernel_end) {
        serial_dump_bytes_unlocked("[cpu] rip bytes", (const uint8_t *)rip, 16);
    }

    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}


int
x86_page_fault_handler(uint64_t error_code)
{
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    uint32_t pid = process_current_pid();
    process_t *proc = process_get(pid);
    if (!proc) {
        return -1;
    }

    if (memory_service_handle_fault_ipc(proc->context_id, cr2, error_code) != 0) {
        serial_write("[cpu] page fault not handled\n");
        return -1;
    }
    return 0;
}

void
cpu_init(void)
{
    serial_write("[cpu] init\n");
    tss_init();
    gdt_install();
    idt_install();
    for (uint32_t i = 0; i < IRQ_COUNT; ++i) {
        uintptr_t handler = (uintptr_t)x86_irq_stub_table[i];
        if (i == 0) {
            idt_set_gate_ist((uint8_t)(IRQ_VECTOR_BASE + i), handler, IDT_TYPE_INTERRUPT_GATE, IRQ0_IST_INDEX);
        } else {
            idt_set_gate((uint8_t)(IRQ_VECTOR_BASE + i), handler, IDT_TYPE_INTERRUPT_GATE);
        }
    }
    irq_init();
    serial_write("[cpu] gdt/idt ready\n");
    (void)KERNEL_DS_SELECTOR;
}

void
cpu_enable_interrupts(void)
{
    __asm__ volatile("sti");
}

void
cpu_disable_interrupts(void)
{
    __asm__ volatile("cli");
}
