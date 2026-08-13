#ifndef WASMOS_SPAWN_INFO_H
#define WASMOS_SPAWN_INFO_H

/*
 * wasmos_spawn_info_t — the process-startup contract.
 *
 * At spawn time the process manager writes one of these headers (immediately
 * followed by the raw argument blob) into a child-owned transfer buffer. The
 * child retrieves it by execution model:
 *   - WASM apps:     wasmos_spawn_info_buffer() hostcall -> buffer_id, then
 *                    xfer_buffer_read() the header + args blob.
 *   - Native apps:   api->spawn_info(&header, args_buf, args_cap) fills the
 *                    header and copies the args blob directly (trusted code,
 *                    no buffer indirection).
 *
 * The header is versioned and forward-extensible: readers validate `magic`,
 * check `version`, and use `header_size` as the cursor to the args blob (never
 * sizeof(struct), so older readers keep working when newer kernels append
 * fields). New fields are APPEND-ONLY at the end of the struct; bump `version`
 * when adding them.
 *
 * Self-contained: depends only on <stdint.h> so it compiles in freestanding
 * kernel, driver, and libc builds.
 */

#include <stdint.h>

/* 'W','S','P','I' packed high byte first, so the bytes read I,P,S,W in memory. */
#define WASMOS_SPAWN_INFO_MAGIC 0x57535049u
#define WASMOS_SPAWN_INFO_VERSION 1u

typedef struct {
    uint32_t magic;       /* WASMOS_SPAWN_INFO_MAGIC */
    uint32_t version;     /* WASMOS_SPAWN_INFO_VERSION */
    uint32_t header_size; /* sizeof(this header); cursor to the args blob */

    uint32_t proc_endpoint; /* process-manager IPC endpoint (svc_lookup bootstrap) */
    uint32_t tty;           /* allocated controlling TTY id, 0 if none */
    uint32_t module_count;  /* number of boot modules (sysinit/device_manager) */
    uint32_t module_index;  /* this module's index in the boot list, 0 if N/A */

    uint32_t args_off; /* byte offset of the args blob within the buffer */
    uint32_t args_len; /* args length in bytes, excluding the trailing NUL */
    /* v2+: env_off/env_len, cwd_off/cwd_len, uid, ... appended here. */
} wasmos_spawn_info_t;

#endif /* WASMOS_SPAWN_INFO_H */