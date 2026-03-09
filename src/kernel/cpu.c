#include "cpu.h"
#include "serial.h"
#include "process.h"
#include "memory_service.h"
#include "irq.h"
#include <stdint.h>

#define GDT_ENTRY_COUNT 3
#define IDT_ENTRY_COUNT 256
#define EXCEPTION_COUNT 32

#define KERNEL_CS_SELECTOR 0x08
#define KERNEL_DS_SELECTOR 0x10

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

static uint64_t g_gdt[GDT_ENTRY_COUNT] = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL,
    0x00AF92000000FFFFULL,
};
static idt_entry_t g_idt[IDT_ENTRY_COUNT];

static void
serial_write_hex64(uint64_t value)
{
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
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
    gdt_install();
    idt_install();
    for (uint32_t i = 0; i < IRQ_COUNT; ++i) {
        uintptr_t handler = (uintptr_t)x86_irq_stub_table[i];
        idt_set_gate((uint8_t)(IRQ_VECTOR_BASE + i), handler, IDT_TYPE_INTERRUPT_GATE);
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
