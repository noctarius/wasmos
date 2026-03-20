#include "ipc.h"
#include "serial.h"
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
 * byte is also sent to the framebuffer driver as FBTEXT_IPC_PUT_CHAR_REQ so
 * live output appears on screen alongside the serial/chardev path. */
#define FBTEXT_IPC_PUT_CHAR_REQ 0x604
static uint32_t g_fb_endpoint = IPC_ENDPOINT_NONE;

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

static int serial_remote_transmit(uint8_t value) {
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

static void serial_fb_transmit(uint8_t c) {
    if (g_fb_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }
    ipc_message_t msg = {
        .type        = FBTEXT_IPC_PUT_CHAR_REQ,
        .source      = IPC_ENDPOINT_NONE,
        .destination = g_fb_endpoint,
        .request_id  = 0,
        .arg0        = (uint32_t)c,
        .arg1        = 0,
        .arg2        = 0,
        .arg3        = 0,
    };
    /* Fire-and-forget: ignore send errors so a crashed/slow framebuffer
     * driver never stalls the kernel console path. */
    (void)ipc_send_from(IPC_CONTEXT_KERNEL, g_fb_endpoint, &msg);
}

static void serial_transmit(char c) {
    if (!serial_remote_transmit((uint8_t)c)) {
        serial_put_internal(c);
    }
    serial_fb_transmit((uint8_t)c);
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

void serial_write_unlocked(const char *s) {
    if (!s) {
        return;
    }
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
