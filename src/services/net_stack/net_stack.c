/* net_stack.c - native (non-WASM) net-stack service entry.
 *
 * COMPILE MILESTONE (compile + link + pack only):
 *   - Captures the wasmos_driver_api_t table for port.c (time/console).
 *   - Validates the native ABI magic/version (mirrors gfx_compositor).
 *   - Calls lwip_init() once to exercise the linked lwIP core.
 *   - Notifies ready and enters a benign idle loop (the native service ABI
 *     expects the entry not to return, matching gfx_compositor).
 *
 * DELIBERATELY NOT DONE YET (later netif/ICMP/socket steps):
 *   - No IPC service loop, no svc_register, no endpoint creation.
 *   - No netif glue, no driver IPC wiring, no socket API, no netstack behavior.
 */
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#include "lwip/init.h"

#include "wasmos_native_driver.h"

/* The api table captured at initialize() time. Read by port.c (sys_now,
 * lwip_port_rand) and net_stack_lwip_diag(). NULL until initialize() runs. */
static wasmos_driver_api_t* g_api = NULL;

wasmos_driver_api_t* net_stack_api(void) {
    return g_api;
}

/* Minimal string length for the raw diag path below. */
static int ns_strlen(const char* s) {
    int n = 0;
    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

/* lwIP LWIP_PLATFORM_DIAG hook (see arch/cc.h). For this compile milestone we
 * only emit the raw format string over the native console hook; varargs are not
 * expanded (no minimal printf is pulled in yet). This is sufficient because the
 * milestone exercises no code paths that emit formatted lwIP diagnostics.
 * TODO(net_stack): route through a real minimal vprintf once the netif step
 * needs formatted diagnostics. */
void net_stack_lwip_diag(const char* fmt, ...) {
    (void)0;
    if (g_api != NULL && g_api->console_write != NULL && fmt != NULL) {
        g_api->console_write(fmt, ns_strlen(fmt));
    }
    /* Consume varargs to keep the signature honest even though unused. */
    va_list ap;
    va_start(ap, fmt);
    va_end(ap);
}

/* Native service entry. Signature/ABI match gfx_compositor's initialize():
 *   int initialize(wasmos_driver_api_t *api, int module_count, int, int)
 * The loader jumps here via ELF e_entry (-e initialize). */
int initialize(wasmos_driver_api_t* driver_api, int module_count, int arg2, int arg3) {
    (void)module_count;
    (void)arg2;
    (void)arg3;

    g_api = driver_api;
    if (driver_api == NULL || driver_api->abi_magic != WASMOS_NATIVE_ABI_MAGIC ||
        driver_api->abi_version != WASMOS_NATIVE_ABI_VERSION) {
        return -2;
    }

    /* Bring up the lwIP core (raw API, NO_SYS). This allocates the static
     * memory pools sized by lwipopts.h. No netif is added yet. */
    lwip_init();

    if (driver_api->console_write != NULL) {
        static const char msg[] = "[net-stack] lwip_init ok\n";
        driver_api->console_write(msg, (int)(sizeof(msg) - 1));
    }

    if (driver_api->proc_notify_ready != NULL) {
        driver_api->proc_notify_ready();
    }

    /* Benign idle loop: mirror gfx_compositor's never-return entry contract
     * without setting up a real IPC service loop yet. */
    for (;;) {
        if (driver_api->sched_yield != NULL) {
            driver_api->sched_yield();
        }
    }

    /* Not reached. */
    return 0;
}
