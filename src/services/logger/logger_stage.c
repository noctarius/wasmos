#include "logger_stage.h"

#include <stdio.h>
#include <string.h>

static const char* logger_stage_tag(const logger_stage_t* st) {
    return st->tag ? st->tag : "[logger]";
}

void logger_stage_truncated(logger_stage_t* st, const char* path, uint32_t limit) {
    st->marker_len = (uint32_t)snprintf(st->marker,
                                        sizeof(st->marker),
                                        "%s %s reached %u bytes and was truncated\n",
                                        logger_stage_tag(st),
                                        path,
                                        (unsigned)limit);
}

int32_t logger_stage_next(logger_stage_t* st, logger_read_fn read, void* user) {
    /* A marker still pending (the truncation notice) is written on its own,
     * before the ring is read again: that read can raise a loss notice, which
     * would otherwise take its place. */
    if (st->marker_len) {
        memcpy(st->out, st->marker, st->marker_len);
        st->out_len = st->marker_len;
        st->marker_len = 0;
        return (int32_t)st->out_len;
    }
    uint64_t want = st->seq;
    int32_t n = read(user, st->read_buf, (int32_t)LOGGER_CHUNK, &st->seq);
    if (n < 0 && st->resumed) {
        /* A resumed position the ring refuses was never this log's -- kenv is
         * writable by anyone -- so read from the oldest byte instead, once. */
        st->resumed = 0;
        st->seq = 0;
        want = 0;
        n = read(user, st->read_buf, (int32_t)LOGGER_CHUNK, &st->seq);
    }
    if (n < 0) {
        return n;
    }
    st->resumed = 0;
    if (st->seq != want) {
        st->marker_len = (uint32_t)snprintf(st->marker,
                                            sizeof(st->marker),
                                            want == 0 ? "%s %llu bytes of earlier "
                                                        "kernel log were not retained\n"
                                                      : "%s %llu bytes of kernel log "
                                                        "lost before they were read\n",
                                            logger_stage_tag(st),
                                            (unsigned long long)(st->seq - want));
    }
    st->out_len = 0;
    if (st->marker_len) {
        memcpy(st->out, st->marker, st->marker_len);
        st->out_len = st->marker_len;
        st->marker_len = 0;
    }
    memcpy(st->out + st->out_len, st->read_buf, (uint32_t)n);
    st->out_len += (uint32_t)n;
    st->seq += (uint64_t)n;
    return (int32_t)st->out_len;
}

/* A short count is a full filesystem (tmpfs reports its block pool running out
 * this way). The ring has already been read past the whole chunk, so a short
 * write leaves a hole the file cannot show; the caller stops instead. */
int logger_stage_written(uint32_t staged, int32_t written) {
    return written >= 0 && (uint32_t)written == staged;
}

void logger_stage_resume(logger_stage_t* st, const char* saved, uint32_t len) {
    uint64_t seq = 0;
    if (!st || !saved || len == 0u || len > 20u) {
        return;
    }
    for (uint32_t i = 0; i < len; ++i) {
        uint64_t digit;
        if (saved[i] < '0' || saved[i] > '9') {
            return;
        }
        digit = (uint64_t)(saved[i] - '0');
        if (seq > (UINT64_MAX - digit) / 10u) {
            return;
        }
        seq = seq * 10u + digit;
    }
    st->seq = seq;
    st->resumed = 1;
}

uint32_t logger_stage_save(const logger_stage_t* st, char* out, uint32_t cap) {
    char digits[20];
    uint32_t n = 0;
    uint64_t seq;
    if (!st || !out) {
        return 0;
    }
    seq = st->seq;
    do {
        digits[n++] = (char)('0' + (seq % 10u));
        seq /= 10u;
    } while (seq != 0u);
    if (n + 1u > cap) {
        return 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = digits[n - 1u - i];
    }
    out[n] = '\0';
    return n;
}
