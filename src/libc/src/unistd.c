/* unistd.c - POSIX file I/O over FS IPC: open/read/write/close/stat/fopen etc.
 * File-descriptor operations funnel through libc_fs_request or
 * libc_fs_request_stream, which talk to the FS manager service through an owned
 * transfer buffer. The three standard descriptors are the exception: read on
 * STDIN_FILENO and write on STDOUT_FILENO/STDERR_FILENO go straight to the
 * console host calls and never reach the FS manager. */
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

#include <stddef.h>
#include <stdint.h>

#define WASMOS_FILE_STREAM_COUNT 8
#define WASMOS_FILE_MODE_READ 0x1
#define WASMOS_FILE_MODE_WRITE 0x2

static int32_t g_fs_reply_endpoint = -1;
static int32_t g_fs_request_id = 1;
static FILE g_file_streams[WASMOS_FILE_STREAM_COUNT];
static uint8_t g_file_stream_used[WASMOS_FILE_STREAM_COUNT];

static int32_t libc_fs_reply_endpoint(void) {
    if (g_fs_reply_endpoint >= 0) {
        return g_fs_reply_endpoint;
    }

    g_fs_reply_endpoint = wasmos_ipc_create_endpoint();
    return g_fs_reply_endpoint;
}

static int32_t libc_fs_endpoint(void) {
    return wasmos_fs_endpoint();
}

/* Owner-push grant: the client owns `buffer_id` and grants the FS manager R|W
 * over it for one request, returning the borrow_id to ship in arg3. fs-manager
 * reborrows this to the backend and unborrows it before replying, so the
 * client's release() afterwards finds no active borrows. Returns b1 (>0) or <0.
 * TODO: narrow rights per op (READ needs W, WRITE needs R) once profiled. */
static int32_t libc_fs_grant(int32_t buffer_id) {
    return wasmos_xfer_buffer_borrow(
        libc_fs_endpoint(), buffer_id, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
}

/* Acquire a per-operation transfer buffer, write the NUL-terminated path into
 * it, and return the buffer_id (or -1). The caller owns the buffer, passes the
 * buffer_id in arg2 of the FS request (the FS side borrows it), and must release
 * it with wasmos_xfer_buffer_release once the reply is received. *out_len is set
 * to the path length (excluding the NUL) for the request's path_len arg. */
static int32_t libc_fs_stage_path(const char* path, size_t* out_len) {
    size_t path_len;
    int32_t bid;

    if (!path) {
        return -1;
    }

    path_len = strlen(path);
    if (path_len == 0 || path_len >= (size_t)wasmos_xfer_buffer_size()) {
        return -1;
    }
    bid = wasmos_xfer_buffer_acquire((int32_t)(path_len + 1u));
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, path, (int32_t)(path_len + 1u), 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (out_len) {
        *out_len = path_len;
    }
    return bid;
}

/* Send an FS IPC request and wait for FS_IPC_RESP; skips unmatched messages.
 * Returns 0 on success and fills out_arg0/out_arg1 from the response. */
static int libc_fs_request(int32_t type, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3,
                           int32_t* out_arg0, int32_t* out_arg1) {
    int32_t fs_endpoint = libc_fs_endpoint();
    int32_t reply_endpoint = libc_fs_reply_endpoint();
    wasmos_ipc_message_t reply;
    int32_t request_id;

    if (fs_endpoint < 0 || reply_endpoint < 0) {
        return -1;
    }

    request_id = g_fs_request_id++;
    if (g_fs_request_id < 1) {
        g_fs_request_id = 1;
    }

    if (wasmos_ipc_send(fs_endpoint, reply_endpoint, type, request_id, arg0, arg1, arg2, arg3) !=
        0) {
        return -1;
    }
    for (;;) {
        if (wasmos_ipc_select_one(reply_endpoint) < 0) {
            return -1;
        }
        wasmos_ipc_message_read_last(&reply);
        if (reply.request_id != request_id) {
            continue;
        }
        if (reply.type != FS_IPC_RESP) {
            return -1;
        }
        break;
    }
    if (out_arg0) {
        *out_arg0 = reply.arg0;
    }
    if (out_arg1) {
        *out_arg1 = reply.arg1;
    }
    return 0;
}

/* Send an FS IPC request and reassemble FS_IPC_STREAM chunks into out[].
 * Each stream message carries 4 bytes in arg0..arg3 (0 = padding after EOF).
 * Returns total bytes written, or -1 on error. */
static ssize_t libc_fs_request_stream(int32_t type, int32_t arg0, int32_t arg1, int32_t arg2,
                                      int32_t arg3, char* out, size_t out_cap) {
    int32_t fs_endpoint = libc_fs_endpoint();
    int32_t reply_endpoint = libc_fs_reply_endpoint();
    wasmos_ipc_message_t reply;
    int32_t request_id;
    size_t out_len = 0;

    if (fs_endpoint < 0 || reply_endpoint < 0 || !out || out_cap == 0) {
        return -1;
    }

    request_id = g_fs_request_id++;
    if (g_fs_request_id < 1) {
        g_fs_request_id = 1;
    }

    if (wasmos_ipc_send(fs_endpoint, reply_endpoint, type, request_id, arg0, arg1, arg2, arg3) !=
        0) {
        return -1;
    }

    for (;;) {
        if (wasmos_ipc_select_one(reply_endpoint) < 0) {
            return -1;
        }
        wasmos_ipc_message_read_last(&reply);
        if (reply.request_id != request_id) {
            continue;
        }
        if (reply.type == FS_IPC_STREAM) {
            int32_t args[4] = {reply.arg0, reply.arg1, reply.arg2, reply.arg3};
            for (int i = 0; i < 4; ++i) {
                char c = (char)(args[i] & 0xFF);
                if (c == '\0') {
                    continue;
                }
                if (out_len + 1 >= out_cap) {
                    out[out_cap - 1] = '\0';
                    return (ssize_t)out_len;
                }
                out[out_len++] = c;
            }
            continue;
        }
        if (reply.type != FS_IPC_RESP || reply.arg0 != 0) {
            return -1;
        }
        out[out_len < out_cap ? out_len : (out_cap - 1)] = '\0';
        return (ssize_t)out_len;
    }
}

int open(const char* path, int flags, ...) {
    size_t path_len;
    int32_t fd = -1;
    int32_t bid;
    int32_t b1;
    int rc;
    int access_mode;

    access_mode = flags & (O_WRONLY | O_RDWR);
    if ((flags & ~(O_WRONLY | O_RDWR | O_CREAT | O_APPEND | O_TRUNC)) != 0) {
        return -1;
    }
    if (access_mode != O_RDONLY && access_mode != O_WRONLY && access_mode != O_RDWR) {
        return -1;
    }
    if ((flags & (O_APPEND | O_TRUNC)) && access_mode == O_RDONLY) {
        return -1;
    }

    bid = libc_fs_stage_path(path, &path_len);
    if (bid < 0) {
        return -1;
    }
    b1 = libc_fs_grant(bid);
    if (b1 < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    rc = libc_fs_request(FS_IPC_OPEN_REQ, (int32_t)path_len, flags, bid, b1, &fd, NULL);
    (void)wasmos_xfer_buffer_release(bid);
    if (rc != 0) {
        return -1;
    }
    return (int)fd;
}

ssize_t read(int fd, void* buf, size_t count) {
    uint8_t* dst = (uint8_t*)buf;
    size_t done = 0;
    size_t chunk_max;
    size_t buffer_cap;
    int32_t bid;
    int32_t b1;
    int failed = 0;

    if (!buf) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (fd == STDIN_FILENO) {
        int32_t got = wasmos_console_read(addr_cast(int32_t, buf), (int32_t)count);
        if (got < 0) {
            return -1;
        }
        return (ssize_t)got;
    }
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        return -1;
    }

    chunk_max = (size_t)wasmos_xfer_buffer_size();
    if (chunk_max == 0) {
        return -1;
    }
    buffer_cap = count < chunk_max ? count : chunk_max;
    /* Own one buffer and grant the FS manager once; reuse both across the whole
     * chunk loop (no re-grant per chunk). release() at the end cascade-revokes
     * the grant — the client never unborrows. */
    bid = wasmos_xfer_buffer_acquire((int32_t)buffer_cap);
    if (bid < 0) {
        return -1;
    }
    b1 = libc_fs_grant(bid);
    if (b1 < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }

    while (done < count) {
        size_t chunk = count - done;
        int32_t got = 0;
        if (chunk > chunk_max) {
            chunk = chunk_max;
        }
        if (libc_fs_request(FS_IPC_READ_REQ, fd, (int32_t)chunk, bid, b1, &got, NULL) != 0 ||
            got < 0 || (size_t)got > chunk) {
            failed = 1;
            break;
        }
        if (got == 0) {
            break;
        }
        if (wasmos_xfer_buffer_read(bid, dst + done, got, 0) != 0) {
            failed = 1;
            break;
        }
        done += (size_t)got;
        if ((size_t)got < chunk) {
            break;
        }
    }

    (void)wasmos_xfer_buffer_release(bid);
    if (failed && done == 0) {
        return -1;
    }
    return (ssize_t)done;
}

ssize_t write(int fd, const void* buf, size_t count) {
    const uint8_t* src = (const uint8_t*)buf;
    size_t done = 0;
    size_t chunk_max;
    size_t buffer_cap;
    int32_t bid;
    int32_t b1;
    int failed = 0;

    if (!buf) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        int32_t wrote = wasmos_console_write(addr_cast(int32_t, buf), (int32_t)count);
        if (wrote < 0) {
            return -1;
        }
        return (ssize_t)count;
    }
    if (fd == STDIN_FILENO) {
        return -1;
    }

    chunk_max = (size_t)wasmos_xfer_buffer_size();
    if (chunk_max == 0) {
        return -1;
    }
    buffer_cap = count < chunk_max ? count : chunk_max;
    /* Own one buffer and grant the FS manager once; reuse both across the whole
     * chunk loop. release() at the end cascade-revokes the grant. */
    bid = wasmos_xfer_buffer_acquire((int32_t)buffer_cap);
    if (bid < 0) {
        return -1;
    }
    b1 = libc_fs_grant(bid);
    if (b1 < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }

    while (done < count) {
        size_t chunk = count - done;
        int32_t wrote = 0;
        if (chunk > chunk_max) {
            chunk = chunk_max;
        }
        if (wasmos_xfer_buffer_write(bid, src + done, (int32_t)chunk, 0) != 0) {
            failed = 1;
            break;
        }
        if (libc_fs_request(FS_IPC_WRITE_REQ, fd, (int32_t)chunk, bid, b1, &wrote, NULL) != 0 ||
            wrote < 0 || (size_t)wrote > chunk) {
            failed = 1;
            break;
        }
        if (wrote == 0) {
            break;
        }
        done += (size_t)wrote;
        if ((size_t)wrote < chunk) {
            break;
        }
    }

    (void)wasmos_xfer_buffer_release(bid);
    if (failed && done == 0) {
        return -1;
    }
    return (ssize_t)done;
}

int close(int fd) {
    return libc_fs_request(FS_IPC_CLOSE_REQ, fd, 0, 0, 0, NULL, NULL);
}

off_t lseek(int fd, off_t offset, int whence) {
    int32_t result = -1;

    if (offset < (off_t)INT32_MIN || offset > (off_t)INT32_MAX) {
        return (off_t)-1;
    }
    if (libc_fs_request(FS_IPC_SEEK_REQ, fd, (int32_t)offset, whence, 0, &result, NULL) != 0) {
        return (off_t)-1;
    }
    return (off_t)result;
}

int stat(const char* path, struct stat* st) {
    size_t path_len;
    int32_t size = 0;
    int32_t mode = 0;
    int32_t bid;
    int32_t b1;
    int rc;

    if (!path || !st) {
        return -1;
    }

    bid = libc_fs_stage_path(path, &path_len);
    if (bid < 0) {
        return -1;
    }
    b1 = libc_fs_grant(bid);
    if (b1 < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    rc = libc_fs_request(FS_IPC_STAT_REQ, (int32_t)path_len, 0, bid, b1, &size, &mode);
    (void)wasmos_xfer_buffer_release(bid);
    if (rc != 0) {
        return -1;
    }

    st->st_size = (uint32_t)size;
    st->st_mode = (uint32_t)mode;
    return 0;
}

/* Send a path-only FS request (unlink/mkdir/rmdir): stage the path into an owned
 * buffer, pass its buffer_id in arg2, release after the reply. */
static int libc_fs_path_op(int32_t type, const char* path) {
    size_t path_len;
    int32_t bid;
    int32_t b1;
    int rc;

    bid = libc_fs_stage_path(path, &path_len);
    if (bid < 0) {
        return -1;
    }
    b1 = libc_fs_grant(bid);
    if (b1 < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    rc = libc_fs_request(type, (int32_t)path_len, 0, bid, b1, NULL, NULL);
    (void)wasmos_xfer_buffer_release(bid);
    return rc;
}

int unlink(const char* path) {
    return libc_fs_path_op(FS_IPC_UNLINK_REQ, path);
}

int rename(const char* old_path, const char* new_path) {
    size_t old_len;
    size_t new_len;
    int32_t bid;
    int32_t b1;
    int rc;

    if (!old_path || !new_path) {
        return -1;
    }
    old_len = strlen(old_path);
    new_len = strlen(new_path);
    if (old_len == 0 || new_len == 0) {
        return -1;
    }
    /* Both paths travel in one buffer, each NUL-terminated: source at 0,
     * destination at old_len + 1.  fat_op_rename derives the second offset
     * rather than being told it, so the two cannot disagree. */
    if (old_len + new_len + 2u > (size_t)wasmos_xfer_buffer_size()) {
        return -1;
    }
    bid = wasmos_xfer_buffer_acquire((int32_t)(old_len + new_len + 2u));
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, old_path, (int32_t)(old_len + 1u), 0) != 0 ||
        wasmos_xfer_buffer_write(bid, new_path, (int32_t)(new_len + 1u), (int32_t)(old_len + 1u)) !=
            0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    b1 = libc_fs_grant(bid);
    if (b1 < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    rc =
        libc_fs_request(FS_IPC_RENAME_REQ, (int32_t)old_len, (int32_t)new_len, bid, b1, NULL, NULL);
    (void)wasmos_xfer_buffer_release(bid);
    return rc;
}

int mkdir(const char* path, mode_t mode) {
    /* TODO: Honor mode bits if WASMOS grows real permission semantics. */
    (void)mode;

    return libc_fs_path_op(FS_IPC_MKDIR_REQ, path);
}

int rmdir(const char* path) {
    return libc_fs_path_op(FS_IPC_RMDIR_REQ, path);
}

ssize_t listdir(char* buf, size_t count) {
    return libc_fs_request_stream(FS_IPC_READDIR_REQ, 0, 0, 0, 0, buf, count);
}

FILE* fopen(const char* path, const char* mode) {
    int fd;
    int open_flags = 0;
    int stream_mode = 0;

    if (!mode) {
        return NULL;
    }
    if (strcmp(mode, "r") == 0 || strcmp(mode, "rb") == 0) {
        open_flags = O_RDONLY;
        stream_mode = WASMOS_FILE_MODE_READ;
    } else if (strcmp(mode, "w") == 0 || strcmp(mode, "wb") == 0) {
        open_flags = O_WRONLY | O_CREAT | O_TRUNC;
        stream_mode = WASMOS_FILE_MODE_WRITE;
    } else if (strcmp(mode, "a") == 0 || strcmp(mode, "ab") == 0) {
        open_flags = O_WRONLY | O_CREAT | O_APPEND;
        stream_mode = WASMOS_FILE_MODE_WRITE;
    } else {
        /* TODO: Extend stdio mode parsing for update modes such as r+/w+/a+. */
        return NULL;
    }

    fd = open(path, open_flags);
    if (fd < 0) {
        return NULL;
    }

    for (size_t i = 0; i < WASMOS_FILE_STREAM_COUNT; ++i) {
        if (!g_file_stream_used[i]) {
            g_file_stream_used[i] = 1;
            g_file_streams[i].fd = fd;
            g_file_streams[i].mode = stream_mode;
            g_file_streams[i].eof = 0;
            g_file_streams[i].error = 0;
            return &g_file_streams[i];
        }
    }

    close(fd);
    return NULL;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    ssize_t rc;
    size_t total;
    size_t done;

    if (!ptr || !stream || stream->fd < 0 || size == 0 || nmemb == 0) {
        return 0;
    }
    if ((stream->mode & WASMOS_FILE_MODE_READ) == 0) {
        stream->error = 1;
        return 0;
    }

    total = size * nmemb;
    /* Refill until the request is met. A single read() short of `total` is not
     * end of file -- a console or pipe backend returns what it has -- so the
     * old one-shot form both misreported a short read as EOF and dropped the
     * bytes of a trailing partial item without saying so. The loop only
     * advances on positive progress, so a backend answering 0 ("nothing ready")
     * terminates it rather than spinning. */
    done = 0;
    while (done < total) {
        rc = read(stream->fd, (char*)ptr + done, total - done);
        if (rc < 0) {
            stream->error = 1;
            break;
        }
        if (rc == 0) {
            stream->eof = 1;
            break;
        }
        done += (size_t)rc;
    }
    /* Bytes past the last whole item are consumed and unreportable -- C leaves a
     * partially read element indeterminate -- so a caller that must not lose
     * them reads in units of 1. */
    return done / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    ssize_t rc;
    size_t total;
    size_t done;

    if (!ptr || !stream || stream->fd < 0 || size == 0 || nmemb == 0) {
        return 0;
    }
    if ((stream->mode & WASMOS_FILE_MODE_WRITE) == 0) {
        stream->error = 1;
        return 0;
    }

    total = size * nmemb;
    /* Drain the whole request. A short write is a failure to store what the
     * caller handed over, so it raises the error flag: the old form reported it
     * only as a smaller item count, which a caller cannot tell apart from
     * having asked for fewer items. As in fread, the loop advances only on
     * positive progress, so a backend that accepts nothing ends it. */
    done = 0;
    while (done < total) {
        rc = write(stream->fd, (const char*)ptr + done, total - done);
        if (rc <= 0) {
            stream->error = 1;
            break;
        }
        done += (size_t)rc;
    }
    if (done < total) {
        stream->error = 1;
    }
    return done / size;
}

int fclose(FILE* stream) {
    if (!stream) {
        return -1;
    }

    for (size_t i = 0; i < WASMOS_FILE_STREAM_COUNT; ++i) {
        if (&g_file_streams[i] == stream && g_file_stream_used[i]) {
            int rc = close(stream->fd);
            g_file_stream_used[i] = 0;
            stream->fd = -1;
            stream->mode = 0;
            stream->eof = 0;
            stream->error = 0;
            return rc;
        }
    }

    return -1;
}

int fseek(FILE* stream, long offset, int whence) {
    if (!stream || stream->fd < 0) {
        return -1;
    }
    if (lseek(stream->fd, (off_t)offset, whence) < 0) {
        stream->error = 1;
        return -1;
    }
    stream->eof = 0;
    stream->error = 0;
    return 0;
}

long ftell(FILE* stream) {
    off_t pos;

    if (!stream || stream->fd < 0) {
        return -1L;
    }
    pos = lseek(stream->fd, 0, SEEK_CUR);
    if (pos < 0) {
        stream->error = 1;
        return -1L;
    }
    return (long)pos;
}

int fgetc(FILE* stream) {
    unsigned char ch = 0;
    ssize_t rc;

    if (!stream || stream->fd < 0) {
        return EOF;
    }

    rc = read(stream->fd, &ch, 1u);
    if (rc < 0) {
        stream->error = 1;
        return EOF;
    }
    if (rc == 0) {
        stream->eof = 1;
        return EOF;
    }
    return (int)ch;
}

int getc(FILE* stream) {
    return fgetc(stream);
}

int getchar(void) {
    unsigned char ch = 0;
    ssize_t rc = read(STDIN_FILENO, &ch, 1u);

    if (rc <= 0) {
        return EOF;
    }
    return (int)ch;
}

int putchar(int ch) {
    unsigned char out = (unsigned char)ch;
    ssize_t rc = write(STDOUT_FILENO, &out, 1u);

    if (rc != 1) {
        return EOF;
    }
    return (int)out;
}

int fputs(const char* s, FILE* stream) {
    size_t len;
    ssize_t rc;

    if (!s || !stream || stream->fd < 0) {
        return -1;
    }
    len = strlen(s);
    rc = write(stream->fd, s, len);
    if (rc < 0) {
        stream->error = 1;
        return -1;
    }
    if ((size_t)rc < len) {
        stream->error = 1;
        return -1;
    }
    return 0;
}

int readline(char* s, int size) {
    int pos = 0;

    if (!s || size <= 1) {
        return -1;
    }

    while (pos + 1 < size) {
        unsigned char ch = 0;
        ssize_t rc = read(STDIN_FILENO, &ch, 1u);
        if (rc < 0) {
            s[0] = '\0';
            return -1;
        }
        if (rc == 0) {
            break;
        }
        s[pos++] = (char)ch;
        if (ch == '\n') {
            break;
        }
    }
    s[pos] = '\0';
    return pos;
}

char* fgets(char* s, int size, FILE* stream) {
    int pos = 0;

    if (!s || size <= 0 || !stream) {
        return NULL;
    }

    while (pos + 1 < size) {
        int ch = fgetc(stream);
        if (ch == EOF) {
            break;
        }
        s[pos++] = (char)ch;
        if (ch == '\n') {
            break;
        }
    }

    if (pos == 0) {
        return NULL;
    }
    s[pos] = '\0';
    return s;
}

int feof(FILE* stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE* stream) {
    return stream ? stream->error : 0;
}

void clearerr(FILE* stream) {
    if (!stream) {
        return;
    }
    stream->eof = 0;
    stream->error = 0;
}
