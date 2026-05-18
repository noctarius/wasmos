#ifndef WASMOS_LIBC_STDLIB_H
#define WASMOS_LIBC_STDLIB_H

#ifdef __cplusplus
extern "C" {
#endif

int abs(int value);
long labs(long value);
int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **endptr, int base);

#ifdef __cplusplus
}
#endif

#endif
