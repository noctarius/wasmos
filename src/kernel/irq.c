#include "irq.h"
#include "ipc.h"
#include "serial.h"
#include "timer.h"
#include "process.h"
#include "paging.h"

/*
 * irq.c handles PIC setup, IRQ routing, and the minimal interrupt-side work
 * needed to wake the rest of the system. The policy rule is strict: do the
 * smallest safe amount of work in interrupt context, then let the scheduler and
 * regular kernel paths finish the job.
 */

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

typedef struct {
    const char *app_name;
    uint16_t irq_mask;
} irq_route_policy_t;

/* Policy is intentionally explicit and default-deny: capability ownership is
 * necessary but not sufficient for userspace IRQ routing. */
static const irq_route_policy_t g_irq_route_policy[] = {
    { "ata",             (uint16_t)((1u << 14) | (1u << 15)) },
    { "irq-route-allow", (uint16_t)(1u << 1) },
};

static inline uintptr_t irq_alias_ptr(uintptr_t p)
{
    if (serial_high_alias_enabled() && (uint64_t)p < KERNEL_HIGHER_HALF_BASE) {
        p = (uintptr_t)((uint64_t)p + KERNEL_HIGHER_HALF_BASE);
    }
    return p;
}

static inline irq_route_t *irq_routes_ptr(void)
{
    return (irq_route_t *)(void *)irq_alias_ptr((uintptr_t)&g_irq_routes[0]);
}

static inline uint8_t *pic_mask1_slot(void)
{
    return (uint8_t *)(void *)irq_alias_ptr((uintptr_t)&g_pic_mask1);
}

static inline uint8_t *pic_mask2_slot(void)
{
    return (uint8_t *)(void *)irq_alias_ptr((uintptr_t)&g_pic_mask2);
}

static int
str_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int
irq_route_policy_allows(uint32_t context_id, uint32_t irq_line)
{
    if (context_id == IPC_CONTEXT_KERNEL) {
        return 1;
    }
    if (irq_line >= IRQ_COUNT) {
        return 0;
    }

    process_t *proc = process_find_by_context(context_id);
    if (!proc || !proc->name) {
        return 0;
    }

    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_irq_route_policy) / sizeof(g_irq_route_policy[0])); ++i) {
        if (!str_eq(proc->name, g_irq_route_policy[i].app_name)) {
            continue;
        }
        return (g_irq_route_policy[i].irq_mask & (uint16_t)(1u << irq_line)) != 0;
    }
    return 0;
}


void x86_irq_iret_corrupt(const uint64_t *saved, const uint64_t *current) {
    serial_write("[irq] iret frame corrupt\n");
    if (!saved || !current) {
        serial_write("[irq] iret frame ptr invalid\n");
        return;
    }
    serial_printf(
        "[irq] saved rip=%016llx\n"
        "[irq] saved cs=%016llx\n"
        "[irq] saved rflags=%016llx\n"
        "[irq] current rip=%016llx\n"
        "[irq] current cs=%016llx\n"
        "[irq] current rflags=%016llx\n",
        (unsigned long long)saved[0],
        (unsigned long long)saved[1],
        (unsigned long long)saved[2],
        (unsigned long long)current[0],
        (unsigned long long)current[1],
        (unsigned long long)current[2]);
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
    outb(PIC1_DATA, *pic_mask1_slot());
    outb(PIC2_DATA, *pic_mask2_slot());
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
    irq_route_t *routes = irq_routes_ptr();
    uint8_t *mask1_slot = pic_mask1_slot();
    uint8_t *mask2_slot = pic_mask2_slot();
    for (uint32_t i = 0; i < IRQ_COUNT; ++i) {
        routes[i].in_use = 0;
        routes[i].owner_context_id = 0;
        routes[i].endpoint = IPC_ENDPOINT_NONE;
    }

    /* Preserve the pre-existing mask state across the PIC remap so only the
     * lines we explicitly unmask later become active. */
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

    *mask1_slot = mask1;
    *mask2_slot = mask2;
    pic_write_masks();
    serial_write("[irq] pic remapped\n");
}

int irq_mask(uint32_t irq_line) {
    uint8_t *mask1_slot = pic_mask1_slot();
    uint8_t *mask2_slot = pic_mask2_slot();
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }
    if (irq_line < 8) {
        *mask1_slot |= (uint8_t)(1u << irq_line);
    } else {
        *mask2_slot |= (uint8_t)(1u << (irq_line - 8));
    }
    pic_write_masks();
    return 0;
}

int irq_unmask(uint32_t irq_line) {
    uint8_t *mask1_slot = pic_mask1_slot();
    uint8_t *mask2_slot = pic_mask2_slot();
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }
    if (irq_line < 8) {
        *mask1_slot &= (uint8_t)~(1u << irq_line);
    } else {
        *mask2_slot &= (uint8_t)~(1u << (irq_line - 8));
    }
    pic_write_masks();
    return 0;
}

int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint) {
    irq_route_t *routes = irq_routes_ptr();
    if (irq_line >= IRQ_COUNT || endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (!irq_route_policy_allows(context_id, irq_line)) {
        return -1;
    }

    uint32_t owner_context_id = 0;
    if (ipc_endpoint_owner(endpoint, &owner_context_id) != IPC_OK ||
        owner_context_id != context_id) {
        return -1;
    }

    routes[irq_line].in_use = 1;
    routes[irq_line].owner_context_id = context_id;
    routes[irq_line].endpoint = endpoint;
    irq_unmask(irq_line);
    return 0;
}

int irq_unregister(uint32_t context_id, uint32_t irq_line) {
    irq_route_t *routes = irq_routes_ptr();
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }

    irq_route_t *route = &routes[irq_line];
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
    irq_route_t *routes = irq_routes_ptr();
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

    irq_route_t *route = &routes[irq_line];
    /* IRQ0 is special because it drives scheduler accounting before any routed
     * notification endpoints are serviced. */
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
    if (WASMOS_TRACE && !logged) {
        logged = 1;
        trace_write("[irq] frame ptr=");
        trace_do(serial_write_hex64((uint64_t)(uintptr_t)frame));
        if (frame) {
            trace_write("[irq] frame rip=");
            trace_do(serial_write_hex64(frame->rip));
            trace_write("[irq] frame cs=");
            trace_do(serial_write_hex64(frame->cs));
            trace_write("[irq] frame rflags=");
            trace_do(serial_write_hex64(frame->rflags));
        }
    }
    /* The common IRQ handler performs accounting and EOI; the scheduler-facing
     * preemption handoff is a second, explicit step. */
    x86_irq_handler(IRQ_VECTOR_BASE);
    process_preempt_from_irq(frame);
}
