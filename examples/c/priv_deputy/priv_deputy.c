/* priv_deputy - an unprivileged process asking the process manager to spawn a
 * privileged package, priv_probe, which declares svc.control.
 *
 * It holds neither svc.control nor spawn.grant, so the kernel must refuse
 * priv_probe its svc.control: otherwise any process could start a privileged
 * package and steer it with arguments of its choosing. The spawn request itself
 * is answered (the child is created); the refusal comes when the child starts,
 * and the kernel logs it. Log lines go to the kernel log, "[priv-deputy] ...". */
#include <stdint.h>

#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#define PRIV_DEPUTY_WAIT_POLLS 2000

static const char k_path[] = "/boot/apps/priv_probe.wap";
static wasmos_sys_event_loop_t g_loop;
static volatile int g_answered;

static void deputy_log(const char* what, int32_t value) {
    char line[96];
    (void)snprintf(line, sizeof(line), "[priv-deputy] %s %d\n", what, (int)value);
    (void)wasmos_console_write(addr_cast(int32_t, line), (int32_t)strlen(line));
}

static void deputy_on_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    deputy_log("spawn answered type", msg ? msg->type : -1);
    g_answered = 1;
}

int main(void) {
    int32_t proc_ep = wasmos_startup_proc_endpoint();
    int32_t ep = wasmos_ipc_create_endpoint();
    if (proc_ep < 0 || ep < 0) {
        deputy_log("no endpoint", -1);
        return 1;
    }
    /* The process manager reads the path from this buffer while it handles the
     * request, so the buffer is kept until the answer arrives. */
    int32_t path_len = (int32_t)(sizeof(k_path) - 1u);
    int32_t bid = wasmos_xfer_buffer_acquire(path_len + 1);
    if (bid < 0 || wasmos_xfer_buffer_write(bid, k_path, path_len, 0) != 0) {
        deputy_log("buffer setup failed", bid);
        return 1;
    }
    wasmos_sys_event_loop_init(&g_loop, ep, 0x100);
    if (wasmos_sys_intent_send(&g_loop,
                               proc_ep,
                               ep,
                               PROC_IPC_SPAWN_PATH,
                               0,
                               (bid << 12) | (path_len & 0xFFF),
                               0,
                               0,
                               deputy_on_reply,
                               0,
                               0) != 0) {
        deputy_log("spawn request failed", -1);
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    for (int32_t i = 0; i < PRIV_DEPUTY_WAIT_POLLS && !g_answered; ++i) {
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
    }
    (void)wasmos_xfer_buffer_release(bid);
    return g_answered ? 0 : 1;
}
