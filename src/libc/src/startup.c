/* startup.c - WASM process entry point for wasmos_main apps.
 *
 * PM no longer passes startup values through entry-arg registers; they travel
 * in the spawn-info buffer and are read through the wasmos_startup_* accessors
 * in spawn_info.c (always linked). This shim only bridges the kernel's
 * wasmos_main export to the application's main(). It is linked only into
 * wasmos_main apps (WASMOS_APP_STARTUP_SHIM); initialize-entry drivers/services
 * do not use it. */
#include "wasmos/imports.h"
#include "wasmos/api.h"

#include <stdint.h>

static char* g_wasmos_argv[1];

extern int main(int argc, char** argv);

/* WASM export called by PM instead of _start. The four entry-arg registers are
 * retired (always zero); startup values come from the spawn-info buffer via the
 * wasmos_startup_* accessors. */
WASMOS_WASM_EXPORT int32_t wasmos_main(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    g_wasmos_argv[0] = 0;
    int32_t rc = main(0, g_wasmos_argv);
    (void)wasmos_proc_exit(rc);
    return rc;
}