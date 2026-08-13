/* wamos_script - standalone executor for the WAMOS .rc scripting language.
 *
 * This is the "engine" half of the shebang script subsystem: PM delegates a
 * `#!`-prefixed guest script to the wamos-script broker, which returns a plan
 * that launches this host with the guest script path as its argv.  The host
 * drives the shared wasmos_script engine (src/libc/src/script.c) over IPC, so a
 * `.rc` script runs as its own process rather than only inside the interactive
 * shell that also embeds that engine.
 *
 * The guest script path arrives at FS-buffer offset 0 (PM writes the spawn
 * argv there); it must be copied out before any FS call reuses the buffer. */
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/script.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

typedef struct {
    int32_t proc_endpoint;
    int32_t reply_endpoint;
    uint32_t request_id;
} wamos_script_ctx_t;

/* Distinct exit statuses so a failed run reports why (named-error-code policy),
 * rather than collapsing every failure to -1.  0 is success. */
enum {
    WAMOS_SCRIPT_OK = 0,
    WAMOS_SCRIPT_ERR_PATH_READ = -2, /* FS-buffer read of the script path failed */
    WAMOS_SCRIPT_ERR_NO_PATH = -3,   /* broker supplied an empty script path */
    WAMOS_SCRIPT_ERR_ENDPOINT = -4,  /* reply endpoint create / proc endpoint missing */
    WAMOS_SCRIPT_ERR_RUN = -5        /* the script engine reported a run failure */
};

/* Owner-push: acquire an xfer buffer, stage "<path>\0[<args>\0]" into it for a
 * path spawn (PM reads it via ownership), and return the path length (or -1).
 * The acquired buffer_id is returned via *out_bid so the caller can pack it into
 * the spawn arg1 and release it after the reply.  args may be NULL. */
static int32_t wamos_script_write_spawn_buf(const char* path, const char* args,
                                            uint32_t* out_args_len, int32_t* out_bid) {
    uint32_t path_len = path ? (uint32_t)strlen(path) : 0u;
    uint32_t args_len = args ? (uint32_t)strlen(args) : 0u;
    int32_t buf_size = wasmos_xfer_buffer_size();
    uint32_t write_off = path_len + 1u;
    int32_t bid = -1;

    if (out_args_len) {
        *out_args_len = 0u;
    }
    if (out_bid) {
        *out_bid = -1;
    }
    if (path_len == 0u || path_len > 0xFFFu || buf_size <= 0 || path_len >= (uint32_t)buf_size) {
        return -1;
    }
    if (args_len > 0u &&
        (write_off >= (uint32_t)buf_size || args_len > ((uint32_t)buf_size - write_off))) {
        return -1;
    }
    bid = wasmos_xfer_buffer_acquire((int32_t)(write_off + args_len + 1u));
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, path), (int32_t)path_len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (args_len > 0u && wasmos_xfer_buffer_write(bid, addr_cast(int32_t, args), (int32_t)args_len,
                                                  (int32_t)write_off) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (out_args_len) {
        *out_args_len = args_len;
    }
    if (out_bid) {
        *out_bid = bid;
    }
    return (int32_t)path_len;
}

static int32_t wamos_script_spawn(wamos_script_ctx_t* ctx, const char* path, const char* args,
                                  int32_t* out_pid) {
    uint32_t args_len = 0u;
    int32_t bid = -1;
    int32_t path_len = wamos_script_write_spawn_buf(path, args, &args_len, &bid);
    wasmos_ipc_message_t reply;

    if (out_pid) {
        *out_pid = -1;
    }
    if (path_len < 0) {
        return -1;
    }
    if (wasmos_ipc_call(ctx->proc_endpoint, ctx->reply_endpoint, PROC_IPC_SPAWN_PATH,
                        (int32_t)ctx->request_id++, 0,
                        (int32_t)(((uint32_t)bid << 12) | ((uint32_t)path_len & 0xFFFu)),
                        (int32_t)args_len, 0, &reply) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    if (reply.type != PROC_IPC_RESP || (int32_t)reply.arg0 <= 0) {
        return -1;
    }
    if (out_pid) {
        *out_pid = (int32_t)reply.arg0;
    }
    return 0;
}

static int32_t wamos_script_wait(wamos_script_ctx_t* ctx, int32_t pid, int32_t* out_exit) {
    wasmos_ipc_message_t reply;

    if (out_exit) {
        *out_exit = -1;
    }
    if (pid <= 0) {
        return -1;
    }
    if (wasmos_ipc_call(ctx->proc_endpoint, ctx->reply_endpoint, PROC_IPC_WAIT,
                        (int32_t)ctx->request_id++, pid, 0, 0, 0, &reply) != 0) {
        return -1;
    }
    if (reply.type != PROC_IPC_RESP || (int32_t)reply.arg0 != pid) {
        return -1;
    }
    if (out_exit) {
        *out_exit = (int32_t)reply.arg1;
    }
    return 0;
}

static int wamos_script_on_start(void* user, const char* path) {
    int32_t pid = -1;
    (void)wamos_script_spawn((wamos_script_ctx_t*)user, path, 0, &pid);
    return 0;
}

static int wamos_script_on_spawn(void* user, const char* path) {
    return wamos_script_on_start(user, path);
}

static int wamos_script_on_exec(void* user, const char* path, const char* args,
                                int32_t* out_exit_code) {
    wamos_script_ctx_t* ctx = (wamos_script_ctx_t*)user;
    int32_t pid = -1;
    int32_t exit_code = -1;

    if (out_exit_code) {
        *out_exit_code = -1;
    }
    if (wamos_script_spawn(ctx, path, args, &pid) != 0) {
        return -1;
    }
    (void)wamos_script_wait(ctx, pid, &exit_code);
    if (out_exit_code) {
        *out_exit_code = exit_code;
    }
    return 0;
}

static int wamos_script_on_wait_svc(void* user, const char* name) {
    wamos_script_ctx_t* ctx = (wamos_script_ctx_t*)user;
    int32_t endpoint = wasmos_sys_svc_lookup_retry(ctx->proc_endpoint, ctx->reply_endpoint, name,
                                                   (int32_t)ctx->request_id, 256);
    ctx->request_id += 256u;
    return endpoint >= 0 ? 0 : -1;
}

static void wamos_script_on_echo_ex(void* user, const char* text, int newline) {
    (void)user;
    if (text) {
        (void)putsn(text, strlen(text));
    }
    if (newline) {
        (void)putchar('\n');
    }
}

static void wamos_script_on_echo(void* user, const char* text) {
    wamos_script_on_echo_ex(user, text, 1);
}

static int wamos_script_on_export(void* user, const char* name, const char* value) {
    /* Exported vars live in the script state and are inherited by child scripts;
     * the executor keeps no external environment, so this is observe-only. */
    (void)user;
    (void)name;
    (void)value;
    return 0;
}

int main(int argc, char** argv) {
    /* Static (data-segment) storage, not a stack local: the FS-buffer read
     * lands in already-committed linear memory, matching the proven cliArgs
     * pattern and avoiding a first-touch stack-page coherence gap under WARP. */
    static char script_path[128];
    wamos_script_ctx_t ctx;
    wasmos_script_state_t state;
    wasmos_script_ops_t ops = {0};

    (void)argc;
    (void)argv;

    /* The guest script path is this process's argv, delivered in the spawn-info
     * buffer. wasmos_startup_args copies it out of libc's cached copy, so it is
     * safe against the FS transfer buffer being reused later by fopen. */
    (void)wasmos_startup_args(script_path, (uint32_t)sizeof(script_path));
    if (script_path[0] == '\0') {
        puts("[wamos-script] no script path");
        return WAMOS_SCRIPT_ERR_NO_PATH;
    }

    ctx.proc_endpoint = wasmos_startup_arg(0);
    ctx.request_id = 1u;
    ctx.reply_endpoint = wasmos_ipc_create_endpoint();
    if (ctx.reply_endpoint < 0 || ctx.proc_endpoint <= 0) {
        puts("[wamos-script] endpoint setup failed");
        return WAMOS_SCRIPT_ERR_ENDPOINT;
    }

    puts("[wamos-script] run start");
    wasmos_script_state_init(&state);
    ops.on_start = wamos_script_on_start;
    ops.on_spawn = wamos_script_on_spawn;
    ops.on_exec = wamos_script_on_exec;
    ops.on_wait_svc = wamos_script_on_wait_svc;
    ops.on_echo = wamos_script_on_echo;
    ops.on_echo_ex = wamos_script_on_echo_ex;
    ops.on_export = wamos_script_on_export;
    ops.user = &ctx;

    if (wasmos_script_run(&state, &ops, script_path) != 0) {
        wasmos_script_state_dispose(&state);
        puts("[wamos-script] run failed");
        return WAMOS_SCRIPT_ERR_RUN;
    }
    wasmos_script_state_dispose(&state);
    puts("[wamos-script] run done");
    return WAMOS_SCRIPT_OK;
}
