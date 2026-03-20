#ifndef WASMOS_SERIAL_H
#define WASMOS_SERIAL_H

#include <stdint.h>

typedef struct serial_driver {
    void (*init)(void);
    void (*put_char)(char c);
    int (*read_char)(uint8_t *out_char);
} serial_driver_t;

void serial_init(void);
void serial_write(const char *s);
void serial_write_unlocked(const char *s);
void serial_write_hex64(uint64_t value);
void serial_write_hex64_unlocked(uint64_t value);
void serial_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void serial_printf_unlocked(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int serial_read_char(uint8_t *out_char);

const serial_driver_t *serial_set_driver(const serial_driver_t *driver);
const serial_driver_t *serial_get_driver(void);

int serial_register_remote_driver(uint32_t endpoint);
int serial_register_fb_backend(uint32_t context_id, uint32_t endpoint);
uint32_t serial_get_fb_endpoint(void);
void serial_input_push(uint8_t ch);
int  serial_input_read(uint8_t *out);

/* Early log ring buffer — captured from the first serial_write onward.
 * Returns the number of bytes currently buffered (capped at ring size).
 * early_log_copy copies up to len bytes starting at logical offset into dst. */
uint32_t serial_early_log_size(void);
void     serial_early_log_copy(uint8_t *dst, uint32_t offset, uint32_t len);

#ifndef WASMOS_TRACE
#define WASMOS_TRACE 0
#endif

#if WASMOS_TRACE
#define trace_write(s) serial_write(s)
#define trace_write_unlocked(s) serial_write_unlocked(s)
#define trace_do(stmt) do { stmt; } while (0)
#else
#define trace_write(s) ((void)0)
#define trace_write_unlocked(s) ((void)0)
#define trace_do(stmt) ((void)0)
#endif

#endif
