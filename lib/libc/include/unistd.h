#ifndef WASMOS_LIBC_UNISTD_H
#define WASMOS_LIBC_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
int open(const char *path, int flags, ...);

#endif
