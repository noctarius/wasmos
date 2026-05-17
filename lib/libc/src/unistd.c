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
#define WASMOS_FILE_MODE_READ  0x1
#define WASMOS_FILE_MODE_WRITE 0x2

static int32_t g_fs_reply_endpoint = -1;
static int32_t g_fs_request_id = 1;
static FILE g_file_streams[WASMOS_FILE_STREAM_COUNT];
static uint8_t g_file_stream_used[WASMOS_FILE_STREAM_COUNT];

static int32_t
libc_fs_reply_endpoint(void)
{
    if (g_fs_reply_endpoint >= 0) {
        return g_fs_reply_endpoint;
    }

    g_fs_reply_endpoint = wasmos_ipc_create_endpoint();
    return g_fs_reply_endpoint;
}

static int32_t
libc_fs_endpoint(void)
{
    return wasmos_fs_endpoint();
}

static int
libc_fs_stage_path(const char *path, size_t *out_len)
{
    size_t path_len;

    if (!path) {
        return -1;
    }

    path_len = strlen(path);
    if (path_len == 0 || path_len >= (size_t)wasmos_fs_buffer_size()) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, (int32_t)(path_len + 1u), 0) != 0) {
        return -1;
    }
    if (out_len) {
        *out_len = path_len;
    }
    return 0;
}

static int
libc_fs_request(int32_t type,
                int32_t arg0,
                int32_t arg1,
                int32_t arg2,
                int32_t arg3,
                int32_t *out_arg0,
                int32_t *out_arg1)
{
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

    if (wasmos_ipc_send(fs_endpoint,
                        reply_endpoint,
                        type,
                        request_id,
                        arg0,
                        arg1,
                        arg2,
                        arg3) != 0) {
        return -1;
    }
    if (wasmos_ipc_recv(reply_endpoint) < 0) {
        return -1;
    }

    wasmos_ipc_message_read_last(&reply);
    if (reply.request_id != request_id || reply.type != FS_IPC_RESP) {
        return -1;
    }
    if (out_arg0) {
        *out_arg0 = reply.arg0;
    }
    if (out_arg1) {
        *out_arg1 = reply.arg1;
    }
    return 0;
}

static ssize_t
libc_fs_request_stream(int32_t type, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3,
                       char *out, size_t out_cap)
{
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

    if (wasmos_ipc_send(fs_endpoint, reply_endpoint, type, request_id, arg0, arg1, arg2, arg3) != 0) {
        return -1;
    }

    for (;;) {
        if (wasmos_ipc_recv(reply_endpoint) < 0) {
            return -1;
        }
        wasmos_ipc_message_read_last(&reply);
        if (reply.request_id != request_id) {
            continue;
        }
        if (reply.type == FS_IPC_STREAM) {
            int32_t args[4] = { reply.arg0, reply.arg1, reply.arg2, reply.arg3 };
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

int
open(const char *path, int flags, ...)
{
    size_t path_len;
    int32_t fd = -1;
    int access_mode;

    access_mode = flags & O_WRONLY;
    if ((flags & ~(O_WRONLY | O_CREAT | O_APPEND | O_TRUNC)) != 0) {
        return -1;
    }
    if (access_mode != O_RDONLY && access_mode != O_WRONLY) {
        return -1;
    }
    if ((flags & (O_APPEND | O_TRUNC)) && access_mode != O_WRONLY) {
        return -1;
    }

    if (libc_fs_stage_path(path, &path_len) != 0) {
        return -1;
    }
    if (libc_fs_request(FS_IPC_OPEN_REQ, (int32_t)path_len, flags, 0, 0, &fd, NULL) != 0) {
        return -1;
    }
    return (int)fd;
}

ssize_t
read(int fd, void *buf, size_t count)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    size_t chunk_max;

    if (!buf) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (fd == STDIN_FILENO) {
        int32_t got = wasmos_console_read((int32_t)(uintptr_t)buf, (int32_t)count);
        if (got < 0) {
            return -1;
        }
        return (ssize_t)got;
    }
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        return -1;
    }

    chunk_max = (size_t)wasmos_fs_buffer_size();
    if (chunk_max == 0) {
        return -1;
    }

    while (done < count) {
        size_t chunk = count - done;
        int32_t got = 0;
        if (chunk > chunk_max) {
            chunk = chunk_max;
        }
        if (libc_fs_request(FS_IPC_READ_REQ, fd, (int32_t)chunk, 0, 0, &got, NULL) != 0) {
            return done > 0 ? (ssize_t)done : -1;
        }
        if (got < 0 || (size_t)got > chunk) {
            return done > 0 ? (ssize_t)done : -1;
        }
        if (got == 0) {
            break;
        }
        if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)(dst + done), got, 0) != 0) {
            return done > 0 ? (ssize_t)done : -1;
        }
        done += (size_t)got;
        if ((size_t)got < chunk) {
            break;
        }
    }

    return (ssize_t)done;
}

ssize_t
write(int fd, const void *buf, size_t count)
{
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    size_t chunk_max;

    if (!buf) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        int32_t wrote = wasmos_console_write((int32_t)(uintptr_t)buf, (int32_t)count);
        if (wrote < 0) {
            return -1;
        }
        return (ssize_t)count;
    }
    if (fd == STDIN_FILENO) {
        return -1;
    }

    chunk_max = (size_t)wasmos_fs_buffer_size();
    if (chunk_max == 0) {
        return -1;
    }

    while (done < count) {
        size_t chunk = count - done;
        int32_t wrote = 0;
        if (chunk > chunk_max) {
            chunk = chunk_max;
        }
        if (wasmos_fs_buffer_write((int32_t)(uintptr_t)(src + done), (int32_t)chunk, 0) != 0) {
            return done > 0 ? (ssize_t)done : -1;
        }
        if (libc_fs_request(FS_IPC_WRITE_REQ, fd, (int32_t)chunk, 0, 0, &wrote, NULL) != 0) {
            return done > 0 ? (ssize_t)done : -1;
        }
        if (wrote < 0 || (size_t)wrote > chunk) {
            return done > 0 ? (ssize_t)done : -1;
        }
        if (wrote == 0) {
            break;
        }
        done += (size_t)wrote;
        if ((size_t)wrote < chunk) {
            break;
        }
    }

    return (ssize_t)done;
}

int
close(int fd)
{
    return libc_fs_request(FS_IPC_CLOSE_REQ, fd, 0, 0, 0, NULL, NULL);
}

off_t
lseek(int fd, off_t offset, int whence)
{
    int32_t result = -1;

    if (offset < (off_t)INT32_MIN || offset > (off_t)INT32_MAX) {
        return (off_t)-1;
    }
    if (libc_fs_request(FS_IPC_SEEK_REQ,
                        fd,
                        (int32_t)offset,
                        whence,
                        0,
                        &result,
                        NULL) != 0) {
        return (off_t)-1;
    }
    return (off_t)result;
}

int
stat(const char *path, struct stat *st)
{
    size_t path_len;
    int32_t size = 0;
    int32_t mode = 0;

    if (!path || !st) {
        return -1;
    }

    if (libc_fs_stage_path(path, &path_len) != 0) {
        return -1;
    }
    if (libc_fs_request(FS_IPC_STAT_REQ,
                        (int32_t)path_len,
                        0,
                        0,
                        0,
                        &size,
                        &mode) != 0) {
        return -1;
    }

    st->st_size = (uint32_t)size;
    st->st_mode = (uint32_t)mode;
    return 0;
}

int
unlink(const char *path)
{
    size_t path_len;

    if (libc_fs_stage_path(path, &path_len) != 0) {
        return -1;
    }
    return libc_fs_request(FS_IPC_UNLINK_REQ, (int32_t)path_len, 0, 0, 0, NULL, NULL);
}

int
mkdir(const char *path, mode_t mode)
{
    size_t path_len;

    /* TODO: Honor mode bits if WASMOS grows real permission semantics. */
    (void)mode;

    if (libc_fs_stage_path(path, &path_len) != 0) {
        return -1;
    }
    return libc_fs_request(FS_IPC_MKDIR_REQ, (int32_t)path_len, 0, 0, 0, NULL, NULL);
}

int
rmdir(const char *path)
{
    size_t path_len;

    if (libc_fs_stage_path(path, &path_len) != 0) {
        return -1;
    }
    return libc_fs_request(FS_IPC_RMDIR_REQ, (int32_t)path_len, 0, 0, 0, NULL, NULL);
}

ssize_t
listdir(char *buf, size_t count)
{
    return libc_fs_request_stream(FS_IPC_READDIR_REQ, 0, 0, 0, 0, buf, count);
}

FILE *
fopen(const char *path, const char *mode)
{
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

size_t
fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    ssize_t rc;
    size_t total;

    if (!ptr || !stream || stream->fd < 0 || size == 0 || nmemb == 0) {
        return 0;
    }
    if ((stream->mode & WASMOS_FILE_MODE_READ) == 0) {
        stream->error = 1;
        return 0;
    }

    total = size * nmemb;
    rc = read(stream->fd, ptr, total);
    if (rc < 0) {
        stream->error = 1;
        return 0;
    }
    if ((size_t)rc < total) {
        stream->eof = 1;
    }
    return (size_t)rc / size;
}

size_t
fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    ssize_t rc;
    size_t total;

    if (!ptr || !stream || stream->fd < 0 || size == 0 || nmemb == 0) {
        return 0;
    }
    if ((stream->mode & WASMOS_FILE_MODE_WRITE) == 0) {
        stream->error = 1;
        return 0;
    }

    total = size * nmemb;
    rc = write(stream->fd, ptr, total);
    if (rc < 0) {
        stream->error = 1;
        return 0;
    }
    return (size_t)rc / size;
}

int
fclose(FILE *stream)
{
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

int
fseek(FILE *stream, long offset, int whence)
{
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

long
ftell(FILE *stream)
{
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

int
fgetc(FILE *stream)
{
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

int
getc(FILE *stream)
{
    return fgetc(stream);
}

int
getchar(void)
{
    unsigned char ch = 0;
    ssize_t rc = read(STDIN_FILENO, &ch, 1u);

    if (rc <= 0) {
        return EOF;
    }
    return (int)ch;
}

int
putchar(int ch)
{
    unsigned char out = (unsigned char)ch;
    ssize_t rc = write(STDOUT_FILENO, &out, 1u);

    if (rc != 1) {
        return EOF;
    }
    return (int)out;
}

int
fputs(const char *s, FILE *stream)
{
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

int
readline(char *s, int size)
{
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

char *
fgets(char *s, int size, FILE *stream)
{
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

int
feof(FILE *stream)
{
    return stream ? stream->eof : 0;
}

int
ferror(FILE *stream)
{
    return stream ? stream->error : 0;
}

void
clearerr(FILE *stream)
{
    if (!stream) {
        return;
    }
    stream->eof = 0;
    stream->error = 0;
}
