#include <stdarg.h>
#include "console_ring.h"
#include "ipc.h"
#include "memory.h"
#include "serial.h"
#include "stdio.h"

#include "process.h"
#include "spinlock.h"
#include "paging.h"

#define COM1_PORT 0x3F8
#define COM1_STATUS (COM1_PORT + 5)

#define SERIAL_DRIVER_WRITE_REQ 0x500
#define SERIAL_DRIVER_READ_REQ 0x501
#define SERIAL_DRIVER_RESP 0x580
#define SERIAL_DRIVER_ERROR 0x5FF

#define SERIAL_READ_STATUS_CHAR 1
#define SERIAL_READ_STATUS_EMPTY 0
#define SERIAL_READ_STATUS_ERROR (-1)
extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

static inline int serial_ptr_needs_kernel_alias(uintptr_t p)
{
    if (!serial_high_alias_enabled() || p == 0) {
        return 0;
    }
    uint64_t start = (uint64_t)(uintptr_t)&__kernel_start;
    uint64_t end = (uint64_t)(uintptr_t)&__kernel_end;
    return ((uint64_t)p >= start && (uint64_t)p < end) ? 1 : 0;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#define EARLY_LOG_SIZE 4096
static uint8_t  g_early_log[EARLY_LOG_SIZE];
static uint32_t g_early_log_head  = 0;  /* next write index (wraps) */
static uint32_t g_early_log_count = 0;  /* bytes written, capped at EARLY_LOG_SIZE */

static spinlock_t g_serial_lock = {0};
static uint8_t g_serial_high_alias_enabled = 0;
static uint32_t g_console_ring_shmem_id = 0;
static console_ring_t *g_console_ring = 0;

static inline spinlock_t *
serial_lock_ptr(void)
{
    uintptr_t addr = (uintptr_t)&g_serial_lock;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (spinlock_t *)(void *)addr;
}

void serial_enable_high_alias(uint8_t enabled) {
    g_serial_high_alias_enabled = enabled ? 1 : 0;
}

uint8_t serial_high_alias_enabled(void) {
    return g_serial_high_alias_enabled;
}

static inline console_ring_t **
serial_console_ring_slot(void)
{
    uintptr_t addr = (uintptr_t)&g_console_ring;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (console_ring_t **)(void *)addr;
}

static inline uint32_t *
serial_console_ring_id_slot(void)
{
    uintptr_t addr = (uintptr_t)&g_console_ring_shmem_id;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint32_t *)(void *)addr;
}

static inline uint8_t *
serial_early_log_buf(void)
{
    uintptr_t addr = (uintptr_t)&g_early_log[0];
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint8_t *)(void *)addr;
}

static inline uint32_t *
serial_early_log_head_slot(void)
{
    uintptr_t addr = (uintptr_t)&g_early_log_head;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint32_t *)(void *)addr;
}

static inline uint32_t *
serial_early_log_count_slot(void)
{
    uintptr_t addr = (uintptr_t)&g_early_log_count;
    if (g_serial_high_alias_enabled && (uint64_t)addr < KERNEL_HIGHER_HALF_BASE) {
        addr = (uintptr_t)((uint64_t)addr + KERNEL_HIGHER_HALF_BASE);
    }
    return (uint32_t *)(void *)addr;
}

static int serial_tx_ready(void) {
    return (inb(COM1_STATUS) & 0x20) != 0;
}

static int serial_rx_ready(void) {
    return (inb(COM1_STATUS) & 0x01) != 0;
}

static void com1_serial_init(void) {
    outb(COM1_PORT + 1, 0x00); // Disable interrupts
    outb(COM1_PORT + 3, 0x80); // Enable DLAB
    outb(COM1_PORT + 0, 0x01); // Divisor low (115200 baud)
    outb(COM1_PORT + 1, 0x00); // Divisor high
    outb(COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop
    outb(COM1_PORT + 2, 0xC7); // Enable FIFO, clear, 14-byte threshold
    outb(COM1_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

static void com1_serial_put_char(char c) {
    while (!serial_tx_ready()) {
    }
    outb(COM1_PORT, (uint8_t)c);
}

static int com1_serial_read_char(uint8_t *out_char) {
    if (!out_char) {
        return -1;
    }
    if (!serial_rx_ready()) {
        return 0;
    }
    *out_char = inb(COM1_PORT);
    return 1;
}

static const serial_driver_t g_com1_driver = {
    .init = com1_serial_init,
    .put_char = com1_serial_put_char,
    .read_char = com1_serial_read_char,
};

static const serial_driver_t *g_serial_driver = &g_com1_driver;

static uint32_t g_serial_remote_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_serial_remote_reply_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_serial_remote_next_request_id = 1;
static uint32_t g_serial_remote_pending_read_request = 0;

/* Keyboard input ring — fed by vt via serial_input_push; polled via
 * the wasmos_input_read kernel import before falling back to COM1. */
#define INPUT_RING_SIZE 64
static uint8_t  g_input_ring[INPUT_RING_SIZE];
static uint32_t g_input_head  = 0;
static uint32_t g_input_count = 0;

static void serial_remote_reset(void) {
    g_serial_remote_endpoint = IPC_ENDPOINT_NONE;
    g_serial_remote_pending_read_request = 0;
    g_serial_remote_next_request_id = 1;
}

static void serial_ring_init(void) {
    console_ring_t **ring_slot = serial_console_ring_slot();
    uint32_t *ring_id_slot = serial_console_ring_id_slot();
    if (*ring_slot) {
        return;
    }
    uint64_t phys_base = 0;
    if (mm_shared_create(1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                         ring_id_slot, &phys_base) != 0) {
        return;
    }
    if (mm_shared_retain(*ring_id_slot) != 0) {
        *ring_id_slot = 0;
        return;
    }
    *ring_slot = (console_ring_t *)(uintptr_t)phys_base;
    (*ring_slot)->write_pos = 0;
    (*ring_slot)->read_pos = 0;
    (*ring_slot)->capacity = CONSOLE_RING_DATA_SIZE;
    (*ring_slot)->_pad = 0;
}

static uint32_t serial_remote_next_request_id(void) {
    uint32_t value = g_serial_remote_next_request_id++;
    if (g_serial_remote_next_request_id == 0) {
        g_serial_remote_next_request_id = 1;
    }
    return value;
}

static int serial_remote_send_message(uint32_t type,
                                      uint32_t request_id,
                                      uint32_t arg0,
                                      uint32_t arg1) {
    if (g_serial_remote_endpoint == IPC_ENDPOINT_NONE ||
        g_serial_remote_reply_endpoint == IPC_ENDPOINT_NONE) {
        return IPC_ERR_INVALID;
    }

    ipc_message_t req = {
        .type = type,
        .source = g_serial_remote_reply_endpoint,
        .destination = g_serial_remote_endpoint,
        .request_id = request_id,
        .arg0 = arg0,
        .arg1 = arg1,
        .arg2 = 0,
        .arg3 = 0,
    };

    int rc = ipc_send_from(IPC_CONTEXT_KERNEL, g_serial_remote_endpoint, &req);
    if (rc != IPC_OK) {
        if (rc == IPC_ERR_INVALID || rc == IPC_ERR_PERM) {
            serial_remote_reset();
        }
    }
    return rc;
}

static int __attribute__((unused)) serial_remote_transmit(uint8_t value) {
    if (serial_remote_send_message(SERIAL_DRIVER_WRITE_REQ,
                                   serial_remote_next_request_id(),
                                   (uint32_t)value,
                                   0) != IPC_OK) {
        return 0;
    }
    return 1;
}

static int serial_remote_read_char(uint8_t *out_char) {
    if (!out_char || g_serial_remote_endpoint == IPC_ENDPOINT_NONE ||
        g_serial_remote_reply_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }

    if (g_serial_remote_pending_read_request != 0) {
        ipc_message_t resp;
        int rc = ipc_recv_for(IPC_CONTEXT_KERNEL, g_serial_remote_reply_endpoint, &resp);
        if (rc == IPC_OK) {
            if (resp.type != SERIAL_DRIVER_RESP ||
                resp.request_id != g_serial_remote_pending_read_request) {
                serial_remote_reset();
                return -1;
            }
            g_serial_remote_pending_read_request = 0;
            int32_t status = (int32_t)resp.arg1;
            if (status == SERIAL_READ_STATUS_CHAR) {
                *out_char = (uint8_t)resp.arg0;
                return 1;
            }
            if (status == SERIAL_READ_STATUS_EMPTY) {
                return 0;
            }
            return -1;
        }
        if (rc == IPC_EMPTY) {
            return 0;
        }
        serial_remote_reset();
        return -1;
    }

    uint32_t request_id = serial_remote_next_request_id();
    if (serial_remote_send_message(SERIAL_DRIVER_READ_REQ, request_id, 0, 0) != IPC_OK) {
        return -1;
    }
    g_serial_remote_pending_read_request = request_id;
    return 0;
}

void serial_input_push(uint8_t ch) {
    spinlock_lock(serial_lock_ptr());
    if (g_input_count < INPUT_RING_SIZE) {
        uint32_t idx = (g_input_head + g_input_count) % INPUT_RING_SIZE;
        g_input_ring[idx] = ch;
        g_input_count++;
    }
    spinlock_unlock(serial_lock_ptr());
}

int serial_input_read(uint8_t *out) {
    spinlock_lock(serial_lock_ptr());
    if (g_input_count == 0) {
        spinlock_unlock(serial_lock_ptr());
        return 0;
    }
    *out = g_input_ring[g_input_head];
    g_input_head = (g_input_head + 1) % INPUT_RING_SIZE;
    g_input_count--;
    spinlock_unlock(serial_lock_ptr());
    return 1;
}

uint32_t serial_console_ring_id(void) {
    uint32_t *ring_id_slot = serial_console_ring_id_slot();
    if (*ring_id_slot == 0) {
        serial_ring_init();
    }
    return *ring_id_slot;
}

void *serial_console_ring_ptr(void) {
    console_ring_t **ring_slot = serial_console_ring_slot();
    if (!*ring_slot) {
        serial_ring_init();
    }
    return *ring_slot;
}

int serial_register_remote_driver(uint32_t endpoint) {
    if (endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }

    uint32_t owner = 0;
    if (ipc_endpoint_owner(endpoint, &owner) != IPC_OK) {
        return -1;
    }

    if (owner == IPC_CONTEXT_KERNEL) {
        return -1;
    }

    if (g_serial_remote_reply_endpoint == IPC_ENDPOINT_NONE) {
        if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &g_serial_remote_reply_endpoint) != IPC_OK) {
            return -1;
        }
    }

    serial_remote_reset();
    g_serial_remote_endpoint = endpoint;
    return 0;
}

const serial_driver_t *serial_set_driver(const serial_driver_t *driver) {
    const serial_driver_t *prev = g_serial_driver;
    g_serial_driver = driver ? driver : &g_com1_driver;
    return prev;
}

const serial_driver_t *serial_get_driver(void) {
    return g_serial_driver;
}

static void serial_put_internal(char c) {
    /* Ring-3 strict mode must not depend on low-address global driver state.
     * Keep TX path CR3-invariant by using direct COM1 I/O. */
    com1_serial_put_char(c);
}

static void serial_ring_write(const char *s) {
    if (g_serial_high_alias_enabled) {
        /* TODO(ring3): map console ring into strict user-visible kernel window
         * or route framebuffer logging through a CR3-invariant path. */
        return;
    }
    console_ring_t *ring = *serial_console_ring_slot();
    if (!ring || !s) {
        return;
    }
    uint32_t cap = ring->capacity;
    uint32_t wp = ring->write_pos;
    if (cap == 0) {
        return;
    }
    while (*s) {
        ring->data[wp % cap] = (uint8_t)*s++;
        wp++;
    }
    ring->write_pos = wp;
}

static void serial_transmit(char c) {
    /* Always use the direct driver for output.  Per-character IPC to the
     * remote serial service overflows the depth-32 endpoint queue for any
     * string longer than 32 bytes; chars beyond slot 32 fall back to direct
     * COM1 while the earlier chars are still queued, inverting their on-wire
     * order.  The remote endpoint is retained for read requests only.
     * The framebuffer driver consumes a shared-memory console ring instead of
     * per-character IPC forwarding from serial_write. */
    serial_put_internal(c);
}

void serial_init(void) {
    if (g_serial_driver && g_serial_driver->init) {
        g_serial_driver->init();
    }
    serial_ring_init();
}

void serial_write(const char *s) {
    if (!s) {
        return;
    }
    spinlock_lock(serial_lock_ptr());
    serial_write_unlocked(s);
    spinlock_unlock(serial_lock_ptr());
}

void serial_printf(const char *fmt, ...)
{
    char buf[512];
    if (serial_ptr_needs_kernel_alias((uintptr_t)fmt)) {
        fmt = (const char *)(uintptr_t)((uint64_t)(uintptr_t)fmt + KERNEL_HIGHER_HALF_BASE);
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
}

void serial_printf_unlocked(const char *fmt, ...)
{
    char buf[512];
    if (serial_ptr_needs_kernel_alias((uintptr_t)fmt)) {
        fmt = (const char *)(uintptr_t)((uint64_t)(uintptr_t)fmt + KERNEL_HIGHER_HALF_BASE);
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write_unlocked(buf);
}

void serial_write_hex64(uint64_t value)
{
    char buf[20];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n'; buf[19] = '\0';
    serial_write(buf);
}

void serial_write_hex64_unlocked(uint64_t value)
{
    char buf[20];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n'; buf[19] = '\0';
    serial_write_unlocked(buf);
}

void serial_write_unlocked(const char *s) {
    if (!s) {
        return;
    }
    if (g_serial_high_alias_enabled) {
        uintptr_t sp = (uintptr_t)s;
        if (serial_ptr_needs_kernel_alias(sp)) {
            s = (const char *)(uintptr_t)((uint64_t)sp + KERNEL_HIGHER_HALF_BASE);
        }
    }
    if (!*serial_console_ring_slot()) {
        serial_ring_init();
    }
    uint8_t *early_log = serial_early_log_buf();
    uint32_t *early_head = serial_early_log_head_slot();
    uint32_t *early_count = serial_early_log_count_slot();
    preempt_disable();
    serial_ring_write(s);
    for (const char *p = s; *p; ++p) {
        if (*p == '\n') {
            serial_transmit('\r');
            early_log[*early_head] = '\r';
            *early_head = (*early_head + 1) % EARLY_LOG_SIZE;
            if (*early_count < EARLY_LOG_SIZE) { (*early_count)++; }
        }
        serial_transmit(*p);
        early_log[*early_head] = (uint8_t)*p;
        *early_head = (*early_head + 1) % EARLY_LOG_SIZE;
        if (*early_count < EARLY_LOG_SIZE) { (*early_count)++; }
    }
    preempt_enable();
}

uint32_t serial_early_log_size(void) {
    return *serial_early_log_count_slot();
}

void serial_early_log_copy(uint8_t *dst, uint32_t offset, uint32_t len) {
    uint8_t *early_log = serial_early_log_buf();
    uint32_t early_head = *serial_early_log_head_slot();
    uint32_t early_count = *serial_early_log_count_slot();
    if (!dst || offset >= early_count) {
        return;
    }
    if (len > early_count - offset) {
        len = early_count - offset;
    }
    /* Logical index 0 is the oldest byte. */
    uint32_t start = (early_count < EARLY_LOG_SIZE)
                   ? 0
                   : early_head; /* head = oldest when ring is full */
    for (uint32_t i = 0; i < len; ++i) {
        dst[i] = early_log[(start + offset + i) % EARLY_LOG_SIZE];
    }
}

int serial_read_char(uint8_t *out_char) {
    if (!out_char) {
        return -1;
    }
    int rc = serial_remote_read_char(out_char);
    if (rc >= 0) {
        return rc;
    }
    /* Match TX strictness: avoid low-address global driver deref here. */
    return com1_serial_read_char(out_char);
}
