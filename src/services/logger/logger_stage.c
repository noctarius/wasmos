#include "logger_stage.h"

#include <stdio.h>
#include <string.h>

void logger_stage_truncated(logger_stage_t* st, const char* path, uint32_t limit) {
    st->marker_len = (uint32_t)snprintf(st->marker,
                                        sizeof(st->marker),
                                        "[logger] %s reached %u bytes and was truncated\n",
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
    if (n < 0) {
        return n;
    }
    if (st->seq != want) {
        st->marker_len = (uint32_t)snprintf(st->marker,
                                            sizeof(st->marker),
                                            want == 0 ? "[logger] %llu bytes of earlier "
                                                        "kernel log were not retained\n"
                                                      : "[logger] %llu bytes of kernel log "
                                                        "lost before they were read\n",
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
