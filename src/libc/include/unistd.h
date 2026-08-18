/* unistd.h - POSIX-like file I/O declarations backed by FS IPC (open/read/write/close). */
#ifndef WASMOS_LIBC_UNISTD_H
#define WASMOS_LIBC_UNISTD_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `whence` selectors for lseek, passed through to the FS manager unchanged. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
/* The three reserved descriptors. read(STDIN_FILENO) and write(STDOUT_FILENO /
 * STDERR_FILENO) bypass the FS manager and go straight to the console host
 * calls; the reverse pairings (write to stdin, read from stdout/stderr) fail
 * with -1. They are not FS descriptors: close() on them is an ordinary FS
 * request and does not close the console. */
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Close an FS descriptor. Returns 0 on success, -1 if the FS manager is
 * unreachable or answers with anything but a plain FS_IPC_RESP. */
int close(int fd);
/* Read up to `count` bytes into `buf`, in transfer-buffer-sized chunks.
 * `buf` is borrowed for the call. Returns the number of bytes read (0 at end of
 * file, and a short count when the file ends mid-request), or -1 for a NULL
 * `buf`, a stdout/stderr descriptor, or a failure with nothing read yet — a
 * failure part-way through still reports the bytes already delivered.
 * count == 0 returns 0 without contacting the FS manager. Blocks on the FS
 * round-trip. A read on STDIN_FILENO instead takes the console path, which
 * delivers at most one byte per call and returns 0 when no input is waiting —
 * a 0 there means "nothing buffered", not end of input. */
ssize_t read(int fd, void* buf, size_t count);
/* Write up to `count` bytes from `buf`, in transfer-buffer-sized chunks.
 * `buf` is borrowed for the call. Returns the number of bytes accepted (a short
 * count when the FS manager stops accepting), or -1 for a NULL `buf`, a stdin
 * descriptor, or a failure with nothing written yet. count == 0 returns 0.
 * A console write reports `count` on success regardless of the host call's own
 * return, since wasmos_console_write reports status, not a byte count. */
ssize_t write(int fd, const void* buf, size_t count);
/* Open `path` with the fcntl.h flags. Only O_RDONLY/O_WRONLY/O_RDWR plus
 * O_CREAT/O_APPEND/O_TRUNC are accepted; any other bit, a nonsensical access
 * mode, or O_APPEND/O_TRUNC on a read-only open is rejected with -1 before any
 * IPC. The variadic mode argument of POSIX open() is accepted and ignored.
 * Returns the FS manager's descriptor on success, -1 on failure. */
int open(const char* path, int flags, ...);
/* Reposition the descriptor. `offset` must fit in int32 (the IPC argument
 * width); a wider value is refused with (off_t)-1 without contacting the FS
 * manager. Returns the resulting absolute offset, or (off_t)-1 on failure. */
off_t lseek(int fd, off_t offset, int whence);
/* Fill *st (st_size and st_mode only) for `path`. Returns 0 on success, -1 on a
 * NULL argument or any FS failure, leaving *st untouched. */
int stat(const char* path, struct stat* st);
/* Remove a file. Returns 0 on success, -1 on failure. */
int unlink(const char* path);

/* Rename or move `old_path` to `new_path` within one mount.  0 on success, -1
 * otherwise.  Unlike POSIX rename(), an EXISTING destination is refused rather
 * than replaced, and an open source is refused (WASMOS_ERR_FS_BUSY): the
 * backend's descriptors record where a file's directory entry lives.  The
 * file's data is never copied -- only the directory entry moves. */
int rename(const char* old_path, const char* new_path);
/* Remove a directory. Returns 0 on success, -1 on failure. */
int rmdir(const char* path);
/* List the FS manager's current directory into `buf`: one newline-terminated
 * entry per name, reassembled from the four payload bytes of each FS_IPC_STREAM
 * reply (embedded NULs are chunk padding and are dropped). The result is always
 * NUL-terminated. Returns the number of bytes written excluding the terminator,
 * or -1 on a NULL or zero-length buffer or a transport failure. A listing longer
 * than `count` is truncated and reported as the truncated length, not as an
 * error. Blocks until the stream ends with the final FS_IPC_RESP. */
ssize_t listdir(char* buf, size_t count);

#ifdef __cplusplus
}
#endif

#endif
