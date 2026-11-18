/* sv_probe - a unit for exercising the supervisor.
 *
 * Arguments: "<mode> <tag>". <tag> names this probe in its log lines and in
 * the kenv key that remembers an earlier run (kenv is global and outlives a
 * process, so it is how a restarted instance knows it is one).
 *   crash       trap at once, before READY, on every start
 *   crash-once  first start: send READY, then trap; any later start: send
 *               READY and stay up
 * Log lines go to the kernel log, "[sv-probe] <tag> ...". */
#include <stdint.h>

#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

static void probe_log(const char* tag, const char* what) {
    char line[96];
    (void)snprintf(line, sizeof(line), "[sv-probe] %s %s\n", tag, what);
    (void)wasmos_console_write(addr_cast(int32_t, line), (int32_t)strlen(line));
}

static void probe_ready(void) {
    int32_t proc_ep = wasmos_startup_proc_endpoint();
    int32_t ep = wasmos_ipc_create_endpoint();
    if (proc_ep >= 0 && ep >= 0) {
        (void)wasmos_ipc_send(proc_ep, ep, PROC_IPC_NOTIFY_READY, 1, 0, 0, 0, 0);
    }
}

int main(void) {
    char args[64];
    char key[40];
    char seen[4];
    char* tag;
    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    tag = strchr(args, ' ');
    if (!tag) {
        probe_log("?", "usage: sv_probe <crash|crash-once> <tag>");
        return 2;
    }
    *tag++ = '\0';

    if (strcmp(args, "crash") == 0) {
        probe_log(tag, "crashing before READY");
        __builtin_trap();
    }
    if (strcmp(args, "crash-once") == 0) {
        (void)snprintf(key, sizeof(key), "svprobe.%s", tag);
        if (wasmos_env_get(key, (int32_t)strlen(key), seen, (int32_t)sizeof(seen)) < 0) {
            (void)wasmos_env_set(key, (int32_t)strlen(key), "1", 1);
            probe_ready();
            probe_log(tag, "first start; crashing after READY");
            __builtin_trap();
        }
        probe_ready();
        probe_log(tag, "up after a restart");
        wasmos_sys_ipc_recv_loop();
    }
    probe_log(tag, "unknown mode");
    return 2;
}
