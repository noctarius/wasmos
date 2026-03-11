#include "irq.h"
#include "ipc.h"
#include "serial.h"
#include "timer.h"
#include "process.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01
#define PIC_EOI 0x20

typedef struct {
    uint8_t in_use;
    uint32_t owner_context_id;
    uint32_t endpoint;
} irq_route_t;

static irq_route_t g_irq_routes[IRQ_COUNT];
static uint8_t g_pic_mask1 = 0xFF;
static uint8_t g_pic_mask2 = 0xFF;

static void serial_write_hex64_local(uint64_t value) {
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

void x86_irq_iret_corrupt(const uint64_t *saved, const uint64_t *current) {
    serial_write("[irq] iret frame corrupt\n");
    if (!saved || !current) {
        serial_write("[irq] iret frame ptr invalid\n");
        return;
    }
    serial_write("[irq] saved rip=");
    serial_write_hex64_local(saved[0]);
    serial_write("[irq] saved cs=");
    serial_write_hex64_local(saved[1]);
    serial_write("[irq] saved rflags=");
    serial_write_hex64_local(saved[2]);
    serial_write("[irq] current rip=");
    serial_write_hex64_local(current[0]);
    serial_write("[irq] current cs=");
    serial_write_hex64_local(current[1]);
    serial_write("[irq] current rflags=");
    serial_write_hex64_local(current[2]);
}

void x86_irq_ist_corrupt(void) {
    serial_write("[irq] ist stack canary corrupt\n");
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void io_wait(void) {
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0), "Nd"((uint16_t)0x80));
}

static void pic_write_masks(void) {
    outb(PIC1_DATA, g_pic_mask1);
    outb(PIC2_DATA, g_pic_mask2);
}

static void pic_send_eoi(uint32_t irq_line) {
    if (irq_line >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

static int pic_is_spurious(uint32_t irq_line) {
    if (irq_line == 7) {
        outb(PIC1_CMD, 0x0B);
        uint8_t isr = inb(PIC1_CMD);
        return (isr & (1u << 7)) == 0;
    }
    if (irq_line == 15) {
        outb(PIC2_CMD, 0x0B);
        uint8_t isr = inb(PIC2_CMD);
        return (isr & (1u << 7)) == 0;
    }
    return 0;
}

void irq_init(void) {
    for (uint32_t i = 0; i < IRQ_COUNT; ++i) {
        g_irq_routes[i].in_use = 0;
        g_irq_routes[i].owner_context_id = 0;
        g_irq_routes[i].endpoint = IPC_ENDPOINT_NONE;
    }

    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC1_DATA, IRQ_VECTOR_BASE);
    io_wait();
    outb(PIC2_DATA, IRQ_VECTOR_BASE + 8);
    io_wait();
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    g_pic_mask1 = mask1;
    g_pic_mask2 = mask2;
    pic_write_masks();
    serial_write("[irq] pic remapped\n");
}

int irq_mask(uint32_t irq_line) {
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }
    if (irq_line < 8) {
        g_pic_mask1 |= (uint8_t)(1u << irq_line);
    } else {
        g_pic_mask2 |= (uint8_t)(1u << (irq_line - 8));
    }
    pic_write_masks();
    return 0;
}

int irq_unmask(uint32_t irq_line) {
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }
    if (irq_line < 8) {
        g_pic_mask1 &= (uint8_t)~(1u << irq_line);
    } else {
        g_pic_mask2 &= (uint8_t)~(1u << (irq_line - 8));
    }
    pic_write_masks();
    return 0;
}

int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint) {
    if (irq_line >= IRQ_COUNT || endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }

    uint32_t owner_context_id = 0;
    if (ipc_endpoint_owner(endpoint, &owner_context_id) != IPC_OK ||
        owner_context_id != context_id) {
        return -1;
    }

    g_irq_routes[irq_line].in_use = 1;
    g_irq_routes[irq_line].owner_context_id = context_id;
    g_irq_routes[irq_line].endpoint = endpoint;
    irq_unmask(irq_line);
    return 0;
}

int irq_unregister(uint32_t context_id, uint32_t irq_line) {
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }

    irq_route_t *route = &g_irq_routes[irq_line];
    if (!route->in_use) {
        return -1;
    }
    if (context_id != IPC_CONTEXT_KERNEL && route->owner_context_id != context_id) {
        return -1;
    }
    route->in_use = 0;
    route->owner_context_id = 0;
    route->endpoint = IPC_ENDPOINT_NONE;
    irq_mask(irq_line);
    return 0;
}

void x86_irq_handler(uint64_t vector) {
    if (vector < IRQ_VECTOR_BASE || vector >= (IRQ_VECTOR_BASE + IRQ_COUNT)) {
        return;
    }

    uint32_t irq_line = (uint32_t)(vector - IRQ_VECTOR_BASE);
    if (pic_is_spurious(irq_line)) {
        if (irq_line == 7) {
            pic_send_eoi(7);
        } else if (irq_line == 15) {
            outb(PIC1_CMD, PIC_EOI);
        }
        return;
    }

    irq_route_t *route = &g_irq_routes[irq_line];
    if (irq_line == 0) {
        timer_handle_irq();
    }
    if (route->in_use) {
        ipc_notify_from(IPC_CONTEXT_KERNEL, route->endpoint);
    }
    pic_send_eoi(irq_line);
}

void x86_timer_irq_handler(irq_frame_t *frame) {
    static uint8_t logged;
    if (!logged) {
        logged = 1;
        serial_write("[irq] frame ptr=");
        serial_write_hex64_local((uint64_t)(uintptr_t)frame);
        if (frame) {
            serial_write("[irq] frame rip=");
            serial_write_hex64_local(frame->rip);
            serial_write("[irq] frame cs=");
            serial_write_hex64_local(frame->cs);
            serial_write("[irq] frame rflags=");
            serial_write_hex64_local(frame->rflags);
        }
    }
    x86_irq_handler(IRQ_VECTOR_BASE);
    process_preempt_from_irq(frame);
}
