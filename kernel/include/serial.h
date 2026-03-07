#ifndef WASMOS_SERIAL_H
#define WASMOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_write(const char *s);

#endif
