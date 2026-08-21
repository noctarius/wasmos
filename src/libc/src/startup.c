/* startup.c - WASM process entry point for wasmos_main apps.
 *
 * PM does not pass startup values through entry-arg registers; they travel in
 * the spawn-info buffer and are read through the wasmos_startup_* accessors in
 * spawn_info.c (always linked). This shim only bridges the kernel's
 * wasmos_main export to the application's main(). It is linked only into
 * wasmos_main apps (WASMOS_APP_STARTUP_SHIM, and crt1.o in the SDK sysroot);
 * initialize-entry drivers/services do not use it. */
#include "wasmos/imports.h"
#include "wasmos/api.h"
#include "wasmos/startup.h"

#include <stdint.h>

/* Backing store for the argv main() receives. Static rather than automatic
 * because argv must outlive this frame for as long as main runs, and sized to
 * hold the argument string the contract can deliver: spawn_info.c caps the blob
 * at 255 bytes, so a buffer that size never truncates an argument here, and an
 * argument string of single-character tokens cannot exceed 128 of them. */
#define WASMOS_MAIN_ARGS_MAX 256u
#define WASMOS_MAIN_ARGV_MAX 130u

static char g_wasmos_args[WASMOS_MAIN_ARGS_MAX];
static char* g_wasmos_argv[WASMOS_MAIN_ARGV_MAX];

extern int main(int argc, char** argv);

/* WASM export called by PM instead of _start. Takes no arguments: startup values
 * come from the spawn-info buffer via the wasmos_startup_* accessors. argc/argv
 * are built from the argument string, so argv[0] is an empty program-name slot
 * and argv[1] is the first argument (see wasmos_startup_argv). */
WASMOS_WASM_EXPORT int32_t wasmos_main(void) {
    int argc = wasmos_startup_argv(
        g_wasmos_args, WASMOS_MAIN_ARGS_MAX, g_wasmos_argv, WASMOS_MAIN_ARGV_MAX);
    int32_t rc = main(argc, g_wasmos_argv);
    (void)wasmos_proc_exit(rc);
    return rc;
}
