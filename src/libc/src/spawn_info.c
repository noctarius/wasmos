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

/* Read this process's spawn-info header + args blob into static storage, once.
 * Lazy + idempotent: works for main-entry apps and initialize-entry
 * services/drivers alike. Leaves g_spawn_info.magic == 0 if none is available. */
static void
wasmos_startup_load(void)
{
    int32_t bid;
    uint32_t n = 0;

    if (g_spawn_loaded) {
        return;
    }
    g_spawn_loaded = 1;
    bid = wasmos_spawn_info_buffer();
    g_spawn_info.magic = 0u;
    g_spawn_args[0] = '\0';
    if (bid <= 0) {
        return;
    }
    if (wasmos_xfer_buffer_read(bid,
                                (int32_t)(uintptr_t)&g_spawn_info,
                                (int32_t)sizeof(g_spawn_info),
                                0) != 0 ||
        g_spawn_info.magic != WASMOS_SPAWN_INFO_MAGIC) {
        g_spawn_info.magic = 0u;
        return;
    }
    n = g_spawn_info.args_len;
    if (n > WASMOS_STARTUP_ARGS_MAX - 1u) {
        n = WASMOS_STARTUP_ARGS_MAX - 1u;
    }
    if (n > 0u &&
        wasmos_xfer_buffer_read(bid,
                                (int32_t)(uintptr_t)g_spawn_args,
                                (int32_t)n,
                                (int32_t)g_spawn_info.args_off) != 0) {
        n = 0u;
    }
    g_spawn_args[n] = '\0';
}

int32_t
wasmos_startup_arg(uint32_t index)
{
    wasmos_startup_load();
    if (index == 0u) {
        return (int32_t)g_spawn_info.proc_endpoint;
    }
    return 0;
}

int32_t
wasmos_startup_proc_endpoint(void)
{
    wasmos_startup_load();
    return (int32_t)g_spawn_info.proc_endpoint;
}

int32_t
wasmos_startup_tty(void)
{
    wasmos_startup_load();
    return (int32_t)g_spawn_info.tty;
}

uint32_t
wasmos_startup_module_count(void)
{
    wasmos_startup_load();
    return g_spawn_info.module_count;
}

uint32_t
wasmos_startup_module_index(void)
{
    wasmos_startup_load();
    return g_spawn_info.module_index;
}

uint32_t
wasmos_startup_args(char *dst, uint32_t cap)
{
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
