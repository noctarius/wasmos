#include "stdio.h"
#include "string.h"
#include "wasmos/imports.h"

#include <stdint.h>

int
putsn(const char *s, size_t len)
{
    if (!s || len == 0 || len > 0x7FFFFFFFul) {
        return 0;
    }
    return wasmos_console_write((int32_t)(uintptr_t)s, (int32_t)len);
}

int
puts(const char *s)
{
    static const char newline = '\n';
    int rc = putsn(s, strlen(s));

    if (rc < 0) {
        return rc;
    }
    return putsn(&newline, 1);
}
