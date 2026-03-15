#include "serial.h"
#include "spinlock.h"

#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static spinlock_t g_serial_lock = {0};

static int serial_tx_ready(void) {
    return (inb(COM1_PORT + 5) & 0x20) != 0;
}

static int serial_rx_ready(void) {
    return (inb(COM1_PORT + 5) & 0x01) != 0;
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

// TODO: Replace this COM1 stub with the real serial driver once the terminal
// transition is ready.
static const serial_driver_t g_com1_driver = {
    .init = com1_serial_init,
    .put_char = com1_serial_put_char,
    .read_char = com1_serial_read_char,
};

static const serial_driver_t *g_serial_driver = &g_com1_driver;

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
            serial_put_internal('\r');
        }
        serial_put_internal(*p);
    }
}

int serial_read_char(uint8_t *out_char) {
    if (!g_serial_driver || !g_serial_driver->read_char) {
        return -1;
    }
    return g_serial_driver->read_char(out_char);
}
