#ifndef WASMOS_LIBC_UNISTD_H
#define WASMOS_LIBC_UNISTD_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int open(const char *path, int flags, ...);
off_t lseek(int fd, off_t offset, int whence);
int stat(const char *path, struct stat *st);
int unlink(const char *path);
int rmdir(const char *path);
ssize_t listdir(char *buf, size_t count);

#endif
