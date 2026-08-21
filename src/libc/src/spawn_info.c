/* spawn_info.c - process startup contract accessors.
 *
 * Every process (both wasmos_main apps and initialize-entry services/drivers)
 * receives a spawn-info buffer from PM (see wasmos_spawn_info.h): a
 * wasmos_spawn_info_t header followed by the argv blob. These accessors fetch
 * the buffer_id via the wasmos_spawn_info_buffer() hostcall and read the header
 * + args once, lazily, so they work regardless of which entry symbol the
 * component exports. This lives in the always-linked libc core (NOT startup.c,
 * which is only linked into wasmos_main apps). */
#include "wasmos/startup.h"
#include "wasmos/api.h"
#include "wasmos_spawn_info.h"

#include <stdint.h>

#define WASMOS_STARTUP_ARGS_MAX 256u

static wasmos_spawn_info_t g_spawn_info;
static char g_spawn_args[WASMOS_STARTUP_ARGS_MAX];
static int g_spawn_loaded;

/* Zero the whole record. A partial reset is not enough: the accessors below read
 * proc_endpoint/tty/module_* unconditionally, so any field left holding buffer
 * bytes is returned to the caller as a startup value. */
static void wasmos_spawn_info_clear(void) {
    uint8_t* p = ptr_cast(uint8_t, &g_spawn_info);
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof(g_spawn_info); ++i) {
        p[i] = 0u;
    }
}

/* Read this process's spawn-info header + args blob into static storage, once.
 * Lazy + idempotent: works for main-entry apps and initialize-entry
 * services/drivers alike. Leaves the record all-zero when no buffer is
 * available or the header does not carry WASMOS_SPAWN_INFO_MAGIC, so every
 * accessor reports 0 rather than whatever the buffer held. */
static void wasmos_startup_load(void) {
    int32_t bid;
    uint32_t n = 0;

    if (g_spawn_loaded) {
        return;
    }
    g_spawn_loaded = 1;
    bid = wasmos_spawn_info_buffer();
    wasmos_spawn_info_clear();
    g_spawn_args[0] = '\0';
    if (bid <= 0) {
        return;
    }
    if (wasmos_xfer_buffer_read(bid, &g_spawn_info, (int32_t)sizeof(g_spawn_info), 0) != 0 ||
        g_spawn_info.magic != WASMOS_SPAWN_INFO_MAGIC) {
        wasmos_spawn_info_clear();
        return;
    }
    n = g_spawn_info.args_len;
    if (n > WASMOS_STARTUP_ARGS_MAX - 1u) {
        n = WASMOS_STARTUP_ARGS_MAX - 1u;
    }
    if (n > 0u && wasmos_xfer_buffer_read(
                      bid, g_spawn_args, (int32_t)n, (int32_t)g_spawn_info.args_off) != 0) {
        n = 0u;
    }
    g_spawn_args[n] = '\0';
}

int32_t wasmos_startup_arg(uint32_t index) {
    wasmos_startup_load();
    if (index == 0u) {
        return (int32_t)g_spawn_info.proc_endpoint;
    }
    return 0;
}

int32_t wasmos_startup_proc_endpoint(void) {
    wasmos_startup_load();
    return (int32_t)g_spawn_info.proc_endpoint;
}

int32_t wasmos_startup_tty(void) {
    wasmos_startup_load();
    return (int32_t)g_spawn_info.tty;
}

uint32_t wasmos_startup_module_count(void) {
    wasmos_startup_load();
    return g_spawn_info.module_count;
}

uint32_t wasmos_startup_module_index(void) {
    wasmos_startup_load();
    return g_spawn_info.module_index;
}

uint32_t wasmos_startup_args(char* dst, uint32_t cap) {
    uint32_t i = 0;
    wasmos_startup_load();
    if (!dst || cap == 0u) {
        return 0u;
    }
    for (; g_spawn_args[i] != '\0' && i < cap - 1u; ++i) {
        dst[i] = g_spawn_args[i];
    }
    dst[i] = '\0';
    return i;
}

/* True for the two bytes the startup contract treats as argument separators.
 * Deliberately not isspace(): a newline or a form feed inside an argument string
 * is a byte of that argument, not a break between two. */
static int wasmos_startup_is_sep(char c) {
    return c == ' ' || c == '\t';
}

/* Build an argv array from this process's argument string.
 *
 * The startup contract supplies ONE NUL-terminated string holding what followed
 * the command name, never the name itself (see wasmos_spawn_info.h), so argv[0]
 * is an empty program-name slot and argv[1] is the first argument. The slot
 * exists because every C program expects it: without it each argument would
 * appear one index lower than the language says.
 * TODO: wasmos_spawn_info_t carries no program name, so argv[0] cannot be one.
 * Appending a name field (the .wap package name PM already parsed) is the fix,
 * and until then a program that prints argv[0] prints nothing.
 *
 * `buf` receives the tokens, NUL-separated in place, and must stay live for as
 * long as argv is used; `buf_cap` and `argv_max` both bound the result, and
 * argv_max counts the program slot and the NULL terminator. An argument the
 * buffer cannot hold WHOLE is dropped rather than truncated: a silently
 * shortened path or number is a failure nothing downstream can detect, while one
 * fewer argument is visible in argc.
 *
 * Returns argc (>= 1 on success), and 0 without touching anything when buf or
 * argv is NULL, buf_cap is 0, or argv_max leaves no room for the slot and the
 * terminator. argv[argc] is always set to NULL. wasmos_startup_args() is
 * unaffected: the blob is copied, not consumed. */
int wasmos_startup_argv(char* buf, uint32_t buf_cap, char** argv, uint32_t argv_max) {
    uint32_t written;
    uint32_t src_len = 0;
    uint32_t i = 0;
    int cut_last;
    int argc;

    if (!buf || !argv || buf_cap == 0u || argv_max < 2u) {
        return 0;
    }

    written = wasmos_startup_args(buf, buf_cap);
    while (g_spawn_args[src_len] != '\0') {
        ++src_len;
    }
    /* Decide whether the last token in buf is a fragment.  There are TWO places
     * the argument string can be cut -- the load's WASMOS_STARTUP_ARGS_MAX cap
     * into g_spawn_args, and this copy's buf_cap -- and either leaves a partial
     * token at the end.  A cut is harmless when the byte at the cut is itself a
     * separator, because the token ended exactly there.  g_spawn_args and
     * g_spawn_info are readable here because this is the translation unit that
     * owns them. */
    if (src_len > written) {
        cut_last = !wasmos_startup_is_sep(g_spawn_args[written]);
    } else if (g_spawn_info.args_len > src_len) {
        cut_last = src_len > 0u && !wasmos_startup_is_sep(g_spawn_args[src_len - 1u]);
    } else {
        cut_last = 0;
    }

    argv[0] = &buf[written]; /* the NUL terminator: an empty program-name slot */
    argc = 1;
    while (buf[i] != '\0') {
        while (wasmos_startup_is_sep(buf[i])) {
            ++i;
        }
        if (buf[i] == '\0') {
            break;
        }
        if ((uint32_t)argc + 1u >= argv_max) {
            break;
        }
        argv[argc++] = &buf[i];
        while (buf[i] != '\0' && !wasmos_startup_is_sep(buf[i])) {
            ++i;
        }
        if (buf[i] != '\0') {
            buf[i++] = '\0';
        } else if (cut_last) {
            --argc;
        }
    }
    argv[argc] = 0;
    return argc;
}

/* Drop the one-shot cache so a host unit test can serve a second spawn-info blob
 * through the stubbed hostcalls. Not part of the guest API: nothing in a running
 * process may re-read its startup contract, which is fixed at spawn. */
void wasmos_startup_reset_for_test(void) {
    g_spawn_loaded = 0;
}
