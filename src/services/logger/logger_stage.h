#ifndef WASMOS_LOGGER_STAGE_H
#define WASMOS_LOGGER_STAGE_H

#include <stdint.h>

/* What the logger writes next, built from the kernel log ring (logger.c).
 *
 * Each write is one chunk of kernel log, preceded by a marker line when there
 * is something to report: bytes the ring overwrote before they were read, or
 * the file having been truncated. No IPC and no globals: the ring read is a
 * callback, so the staging rules are testable on the host. */

#define LOGGER_CHUNK 2048u
#define LOGGER_MARKER_MAX 128u

/* Reads up to `len` bytes of kernel log from sequence *seq into dst, with
 * kmsg_read's contract: *seq is moved to the first byte copied, and the return
 * is the byte count or a negative status. */
typedef int32_t (*logger_read_fn)(void* user, uint8_t* dst, int32_t len, uint64_t* seq);

typedef struct {
    uint64_t seq; /* sequence number of the next kernel-log byte to read */
    char marker[LOGGER_MARKER_MAX];
    uint32_t marker_len; /* a marker line not yet written */
    uint8_t read_buf[LOGGER_CHUNK];
    char out[LOGGER_MARKER_MAX + LOGGER_CHUNK];
    uint32_t out_len; /* bytes staged in out */
} logger_stage_t;

/* Records that the file was truncated at `limit` bytes; the notice is the next
 * thing written. */
void logger_stage_truncated(logger_stage_t* st, const char* path, uint32_t limit);

/* Stages the next write into st->out. Returns the bytes staged, 0 when there is
 * nothing to write, or a negative status from `read`. */
int32_t logger_stage_next(logger_stage_t* st, logger_read_fn read, void* user);

/* The read position as `len` decimal digits at `saved`, which a restarted logger
 * hands back from where logger_stage_save left it (kenv), so it continues the
 * file instead of appending the ring's whole retained backlog to it again. A
 * value that does not parse leaves the position at 0. */
void logger_stage_resume(logger_stage_t* st, const char* saved, uint32_t len);

/* Writes the read position as decimal digits into out[0..cap), NUL-terminated.
 * Returns the digit count, or 0 when cap is too small. */
uint32_t logger_stage_save(const logger_stage_t* st, char* out, uint32_t cap);

/* 1 when an FS write completion of `written` bytes wrote all `staged` bytes. */
int logger_stage_written(uint32_t staged, int32_t written);

#endif
