/* sv_probe - a unit for exercising the supervisor.
 *
 * Arguments: "<mode> <tag>". <tag> names this probe in its log lines and in
 * the kenv key that remembers an earlier run (kenv is global and outlives a
 * process, so it is how a restarted instance knows it is one).
 *   crash       trap at once, before READY, on every start
 *   crash-once  first start: send READY, then trap; any later start: send
 *               READY and stay up
 *   hang-once   run an event loop that opts in to liveness probes; first
 *               start: after SV_PROBE_HANG_AFTER_MS, park in a nested receive
 *               for good, so probes go unanswered; any later start: stay up
 *   busy        run an event loop that opts in to liveness probes and burn
 *               the CPU in SV_PROBE_BUSY_CHUNK_MS chunks between polls for
 *               SV_PROBE_BUSY_FOR_MS, then idle
 *   stay        send READY, log "up", and stay up
 *   entropy     find "virtio-rng" by name watch, ask it for SV_PROBE_ENTROPY_BYTES
 *               (filled by the device's DMA), log "entropy ok" or "entropy
 *               failed <status>", and exit
 * Log lines go to the kernel log, "[sv-probe] <tag> ...". */
#include <stdint.h>

#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#define SV_PROBE_HANG_AFTER_MS 1500u
#define SV_PROBE_BUSY_FOR_MS 12000u
#define SV_PROBE_BUSY_CHUNK_MS 100u
#define SV_PROBE_ENTROPY_BYTES 16
#define SV_PROBE_ENTROPY_FIND_MS 10000

static wasmos_sys_event_loop_t g_loop;
static volatile uint32_t g_sink;

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

static uint32_t probe_now(void) {
    return (uint32_t)wasmos_sched_ticks() * (1000u / WASMOS_SCHED_TICK_HZ);
}

static void probe_on_ack(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

static void probe_on_unclaimed(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

/* Brings up an event loop on a fresh endpoint and sends READY naming it for
 * liveness probes; the loop answers them itself. Returns 0, or -1. */
static int probe_loop_start(void) {
    int32_t proc_ep = wasmos_startup_proc_endpoint();
    int32_t ep = wasmos_ipc_create_endpoint();
    if (proc_ep < 0 || ep < 0) {
        return -1;
    }
    wasmos_sys_event_loop_init(&g_loop, ep, 0x100);
    (void)wasmos_sys_event_set_default(&g_loop, probe_on_unclaimed, 0);
    return wasmos_sys_intent_send(&g_loop,
                                  proc_ep,
                                  ep,
                                  PROC_IPC_NOTIFY_READY,
                                  ep,
                                  PROC_READY_FLAG_LIVENESS,
                                  0,
                                  0,
                                  probe_on_ack,
                                  0,
                                  0);
}

static wasmos_sys_svc_watch_set_t g_watches;
static wasmos_sys_svc_watch_t g_rng_watch;
static wasmos_sys_random_request_t g_random;
static uint8_t g_entropy[SV_PROBE_ENTROPY_BYTES];
static int g_entropy_done;
static int32_t g_entropy_status;

static void probe_on_random(void* user, int32_t status) {
    (void)user;
    g_entropy_status = status;
    g_entropy_done = 1;
}

static void probe_on_rng_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    wasmos_sys_svc_watch_stop(watch);
    int32_t rc = wasmos_sys_random_bytes_async(
        &g_loop, endpoint, g_entropy, SV_PROBE_ENTROPY_BYTES, &g_random, probe_on_random, 0);
    if (rc != 0) {
        probe_on_random(0, rc);
    }
}

static void probe_on_rng_missing(void* user, wasmos_sys_svc_watch_t* watch, int32_t status) {
    (void)user;
    (void)watch;
    probe_on_random(0, status);
}

/* One entropy request to "virtio-rng", on a loop of the probe's own. Returns
 * the packed status of the request, 0 when the bytes arrived. */
static int32_t probe_entropy(void) {
    int32_t ep = wasmos_ipc_create_endpoint();
    if (ep < 0) {
        return ep;
    }
    wasmos_sys_event_loop_init(&g_loop, ep, 0x100);
    g_loop.poll_timeout_ms = 100;
    if (wasmos_sys_svc_watch_set_init(&g_watches, &g_loop, wasmos_startup_proc_endpoint()) != 0 ||
        wasmos_sys_svc_watch_start(&g_watches,
                                   &g_rng_watch,
                                   "virtio-rng",
                                   SV_PROBE_ENTROPY_FIND_MS,
                                   probe_on_rng_bound,
                                   probe_on_rng_missing,
                                   0) != 0) {
        return WASMOS_ERR_HRNG_NOT_READY;
    }
    while (!g_entropy_done) {
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
        wasmos_sys_svc_watch_set_expire(&g_watches);
    }
    return g_entropy_status;
}

/* Keeps the CPU busy for `ms` without returning to the loop. */
static void probe_burn(uint32_t ms) {
    uint32_t start = probe_now();
    while (probe_now() - start < ms) {
        for (uint32_t i = 0; i < 1000u; ++i) {
            g_sink += i;
        }
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
        probe_log("?", "usage: sv_probe <crash|crash-once|hang-once|busy|stay|entropy> <tag>");
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
    if (strcmp(args, "hang-once") == 0) {
        (void)snprintf(key, sizeof(key), "svprobe.%s", tag);
        int first = wasmos_env_get(key, (int32_t)strlen(key), seen, (int32_t)sizeof(seen)) < 0;
        if (first) {
            (void)wasmos_env_set(key, (int32_t)strlen(key), "1", 1);
        }
        if (probe_loop_start() != 0) {
            probe_log(tag, "cannot start its loop");
            return 2;
        }
        if (!first) {
            probe_log(tag, "up after a restart");
            for (;;) {
                (void)wasmos_sys_event_loop_poll(&g_loop, 8);
            }
        }
        probe_log(tag, "first start; its loop turns");
        uint32_t start = probe_now();
        g_loop.poll_timeout_ms = 100;
        while (probe_now() - start < SV_PROBE_HANG_AFTER_MS) {
            (void)wasmos_sys_event_loop_poll(&g_loop, 8);
        }
        /* The service's loop stops turning: parked in a receive on another
         * endpoint, the way a synchronous call waits for an answer that never
         * comes. Probes queue on the loop's endpoint unanswered. */
        probe_log(tag, "hanging in a nested receive");
        wasmos_sys_ipc_recv_loop();
    }
    if (strcmp(args, "busy") == 0) {
        if (probe_loop_start() != 0) {
            probe_log(tag, "cannot start its loop");
            return 2;
        }
        probe_log(tag, "busy");
        uint32_t start = probe_now();
        g_loop.poll_timeout_ms = 1;
        while (probe_now() - start < SV_PROBE_BUSY_FOR_MS) {
            (void)wasmos_sys_event_loop_poll(&g_loop, 8);
            probe_burn(SV_PROBE_BUSY_CHUNK_MS);
        }
        probe_log(tag, "done being busy");
        g_loop.poll_timeout_ms = 0;
        for (;;) {
            (void)wasmos_sys_event_loop_poll(&g_loop, 8);
        }
    }
    if (strcmp(args, "stay") == 0) {
        probe_ready();
        probe_log(tag, "up");
        wasmos_sys_ipc_recv_loop();
    }
    if (strcmp(args, "entropy") == 0) {
        int32_t rc = probe_entropy();
        char what[40];
        if (rc == 0) {
            probe_log(tag, "entropy ok");
            return 0;
        }
        (void)snprintf(what, sizeof(what), "entropy failed %d", (int)rc);
        probe_log(tag, what);
        return 1;
    }
    probe_log(tag, "unknown mode");
    return 2;
}
