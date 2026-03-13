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
libc_fs_request(int32_t type,
                int32_t arg0,
                int32_t arg1,
                int32_t arg2,
                int32_t arg3,
                int32_t *out_arg0)
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
    return 0;
}

int
open(const char *path, int flags, ...)
{
    size_t path_len;
    int32_t fd = -1;

    if (!path || flags != O_RDONLY) {
        return -1;
    }

    path_len = strlen(path);
    if (path_len == 0 || path_len >= (size_t)wasmos_fs_buffer_size()) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, (int32_t)(path_len + 1u), 0) != 0) {
        return -1;
    }
    if (libc_fs_request(FS_IPC_OPEN_REQ, (int32_t)path_len, flags, 0, 0, &fd) != 0) {
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
        if (libc_fs_request(FS_IPC_READ_REQ, fd, (int32_t)chunk, 0, 0, &got) != 0) {
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

int
close(int fd)
{
    return libc_fs_request(FS_IPC_CLOSE_REQ, fd, 0, 0, 0, NULL);
}

FILE *
fopen(const char *path, const char *mode)
{
    int fd;

    if (!mode || (strcmp(mode, "r") != 0 && strcmp(mode, "rb") != 0)) {
        return NULL;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    for (size_t i = 0; i < WASMOS_FILE_STREAM_COUNT; ++i) {
        if (!g_file_stream_used[i]) {
            g_file_stream_used[i] = 1;
            g_file_streams[i].fd = fd;
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
            stream->eof = 0;
            stream->error = 0;
            return rc;
        }
    }

    return -1;
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
