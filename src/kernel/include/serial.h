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
int serial_read_char(uint8_t *out_char);

const serial_driver_t *serial_set_driver(const serial_driver_t *driver);
const serial_driver_t *serial_get_driver(void);

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
