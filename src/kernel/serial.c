#include <stdarg.h>
#include "ipc.h"
#include "serial.h"
#include "stdio.h"

#include "process.h"
#include "spinlock.h"

#define COM1_PORT 0x3F8
#define COM1_STATUS (COM1_PORT + 5)

#define SERIAL_DRIVER_WRITE_REQ 0x500
#define SERIAL_DRIVER_READ_REQ 0x501
#define SERIAL_DRIVER_RESP 0x580
#define SERIAL_DRIVER_ERROR 0x5FF

#define SERIAL_READ_STATUS_CHAR 1
#define SERIAL_READ_STATUS_EMPTY 0
#define SERIAL_READ_STATUS_ERROR (-1)

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

/* Optional framebuffer console backend.  When registered, every transmitted
 * string is also sent to the framebuffer driver in 4-byte chunks via
 * FBTEXT_IPC_PUT_STRING_REQ so live output appears on screen.
 * fbtext_put_char handles '\n' natively, so no \r injection is needed. */
#define FBTEXT_IPC_PUT_STRING_REQ 0x605
static uint32_t g_fb_endpoint = IPC_ENDPOINT_NONE;

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
    spinlock_lock(&g_serial_lock);
    if (g_input_count < INPUT_RING_SIZE) {
        uint32_t idx = (g_input_head + g_input_count) % INPUT_RING_SIZE;
        g_input_ring[idx] = ch;
        g_input_count++;
    }
    spinlock_unlock(&g_serial_lock);
}

int serial_input_read(uint8_t *out) {
    spinlock_lock(&g_serial_lock);
    if (g_input_count == 0) {
        spinlock_unlock(&g_serial_lock);
        return 0;
    }
    *out = g_input_ring[g_input_head];
    g_input_head = (g_input_head + 1) % INPUT_RING_SIZE;
    g_input_count--;
    spinlock_unlock(&g_serial_lock);
    return 1;
}

int serial_register_fb_backend(uint32_t context_id, uint32_t endpoint) {
    (void)context_id;
    if (endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    g_fb_endpoint = endpoint;
    return 0;
}

uint32_t serial_get_fb_endpoint(void) {
    return g_fb_endpoint;
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
    if (g_serial_driver && g_serial_driver->put_char) {
        g_serial_driver->put_char(c);
    }
}

static void serial_fb_transmit_string(const char *s) {
    if (g_fb_endpoint == IPC_ENDPOINT_NONE || !s) {
        return;
    }
    /* Batch up to 4 bytes per FBTEXT_IPC_PUT_STRING_REQ to stay within the
     * depth-32 IPC queue for strings of up to 128 bytes before overflow. */
    while (*s) {
        ipc_message_t msg = {
            .type        = FBTEXT_IPC_PUT_STRING_REQ,
            .source      = IPC_ENDPOINT_NONE,
            .destination = g_fb_endpoint,
            .request_id  = 0,
            .arg0        = 0,
            .arg1        = 0,
            .arg2        = 0,
            .arg3        = 0,
        };
        uint32_t *args = &msg.arg0;
        for (int i = 0; i < 4 && *s; ++i) {
            args[i] = (uint32_t)(uint8_t)*s++;
        }
        (void)ipc_send_from(IPC_CONTEXT_KERNEL, g_fb_endpoint, &msg);
    }
}

static void serial_transmit(char c) {
    /* Always use the direct driver for output.  Per-character IPC to the
     * remote serial service overflows the depth-32 endpoint queue for any
     * string longer than 32 bytes; chars beyond slot 32 fall back to direct
     * COM1 while the earlier chars are still queued, inverting their on-wire
     * order.  The remote endpoint is retained for read requests only.
     * Framebuffer output is batched string-by-string in serial_write_unlocked
     * via serial_fb_transmit_string to avoid the same overflow. */
    serial_put_internal(c);
}

void serial_init(void) {
    if (g_serial_driver && g_serial_driver->init) {
        g_serial_driver->init();
    }
}

void serial_write(const char *s) {
    if (!s) {
        return;
    }
    spinlock_lock(&g_serial_lock);
    serial_write_unlocked(s);
    spinlock_unlock(&g_serial_lock);
}

void serial_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
}

void serial_printf_unlocked(const char *fmt, ...)
{
    char buf[512];
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
    preempt_disable();
    /* Batch-send the string to the framebuffer before iterating per-char.
     * fbtext_put_char handles '\n' natively so no \r expansion is needed. */
    serial_fb_transmit_string(s);
    for (const char *p = s; *p; ++p) {
        if (*p == '\n') {
            serial_transmit('\r');
            g_early_log[g_early_log_head] = '\r';
            g_early_log_head = (g_early_log_head + 1) % EARLY_LOG_SIZE;
            if (g_early_log_count < EARLY_LOG_SIZE) { g_early_log_count++; }
        }
        serial_transmit(*p);
        g_early_log[g_early_log_head] = (uint8_t)*p;
        g_early_log_head = (g_early_log_head + 1) % EARLY_LOG_SIZE;
        if (g_early_log_count < EARLY_LOG_SIZE) { g_early_log_count++; }
    }
    preempt_enable();
}

uint32_t serial_early_log_size(void) {
    return g_early_log_count;
}

void serial_early_log_copy(uint8_t *dst, uint32_t offset, uint32_t len) {
    if (!dst || offset >= g_early_log_count) {
        return;
    }
    if (len > g_early_log_count - offset) {
        len = g_early_log_count - offset;
    }
    /* Logical index 0 is the oldest byte. */
    uint32_t start = (g_early_log_count < EARLY_LOG_SIZE)
                   ? 0
                   : g_early_log_head; /* head = oldest when ring is full */
    for (uint32_t i = 0; i < len; ++i) {
        dst[i] = g_early_log[(start + offset + i) % EARLY_LOG_SIZE];
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
    if (!g_serial_driver || !g_serial_driver->read_char) {
        return -1;
    }
    return g_serial_driver->read_char(out_char);
}
