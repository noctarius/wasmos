#ifndef WASMOS_SERIAL_H
#define WASMOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_write(const char *s);
int serial_read_char(uint8_t *out_char);

#endif
