extern "C" {
#include "process_manager.h"
#include "klog.h"
#include "process_manager_internal.h"
#include "ipc.h"
}

#ifndef WASMOS_PM_LIST_IMPL
#define WASMOS_PM_LIST_IMPL LIST_IMPL_LINKED
#endif

#ifndef WASMOS_PM_LIST_ARRAY_CHUNK_CAP
#define WASMOS_PM_LIST_ARRAY_CHUNK_CAP 16u
#endif

#ifndef WASMOS_PM_TEST_HOOKS
#define WASMOS_PM_TEST_HOOKS 0
#endif

/* Idle poll interval: when no request is queued the entry blocks on its select
 * set for at most this long, bounding the latency of the exit-driven periodic
 * work (pm_check_waits / pm_reap_apps), which no IPC wakes the entry for. */
#ifndef WASMOS_PM_POLL_INTERVAL_MS
#define WASMOS_PM_POLL_INTERVAL_MS 50u
#endif

pm_state_t g_pm;
uint8_t g_pm_wait_owner_deny_logged;
uint8_t g_pm_kill_owner_deny_logged;
uint8_t g_pm_status_owner_deny_logged;
uint8_t g_pm_spawn_owner_deny_logged;

class ProcessManager {
  public:
    typedef struct {
        uint32_t type;
        uint32_t request_id;
        uint32_t arg0;
        uint32_t arg1;
        uint32_t arg2;
        uint32_t arg3;
        uint32_t expected_owner_context_id;
        uint8_t wait_owner_mismatch;
    } inject_request_t;

    pm_wait_state_t* wait_slot_acquire(void) {
        list_iter_t it;
        pm_wait_state_t* waiter = (pm_wait_state_t*)list_first(&g_pm.waits, &it);
        while (waiter) {
            if (!waiter->in_use) {
                return waiter;
            }
            waiter = (pm_wait_state_t*)list_next(&it);
        }
        waiter = (pm_wait_state_t*)list_alloc(&g_pm.waits);
        return waiter;
    }

    /* Round-robin the controlling tty handed to WANTS_TTY children over 1..3;
     * tty 0 is not handed out.  An out-of-range cursor folds back to 1. */
    uint32_t alloc_cli_tty(void) {
        uint32_t tty = g_pm.next_cli_tty;
        if (tty < 1 || tty > 3) {
            tty = 1;
        }
        g_pm.next_cli_tty = (tty >= 3) ? 1 : (tty + 1);
        return tty;
    }

    void inject(const inject_request_t* request) {
#if WASMOS_PM_TEST_HOOKS
        if (!request) {
            return;
        }
        if (request->wait_owner_mismatch) {
            uint32_t reply_endpoint = IPC_ENDPOINT_NONE;
            if (request->expected_owner_context_id == 0) {
                return;
            }
            if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &reply_endpoint) != IPC_OK ||
                reply_endpoint == IPC_ENDPOINT_NONE) {
                return;
            }
            pm_wait_state_t* waiter = wait_slot_acquire();
            if (!waiter) {
                return;
            }
            waiter->in_use = 1;
            waiter->pid = 0;
            waiter->reply_endpoint = reply_endpoint;
            waiter->request_id = request->request_id;
            waiter->owner_context_id = request->expected_owner_context_id;
            return;
        }
        uint32_t source_endpoint = IPC_ENDPOINT_NONE;
        ipc_message_t msg;

        uint32_t proc_endpoint = pm_atomic_load_u32(&g_pm.proc_endpoint);
        if (proc_endpoint == IPC_ENDPOINT_NONE) {
            return;
        }
        if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &source_endpoint) != IPC_OK ||
            source_endpoint == IPC_ENDPOINT_NONE) {
            return;
        }

        msg.type = request->type;
        msg.source = source_endpoint;
        msg.destination = proc_endpoint;
        msg.request_id = request->request_id;
        msg.arg0 = request->arg0;
        msg.arg1 = request->arg1;
        msg.arg2 = request->arg2;
        msg.arg3 = request->arg3;
        (void)ipc_send_from(IPC_CONTEXT_KERNEL, proc_endpoint, &msg);
#else
        (void)request;
#endif
    }

    int init(const boot_info_t* boot_info) {
        g_pm.init_module_index = 0xFFFFFFFFu;
        g_pm.module_count = 0;
        g_pm.boot_info = boot_info;
        pm_atomic_store_u32(&g_pm.proc_endpoint, IPC_ENDPOINT_NONE);
        pm_atomic_store_u32(&g_pm.fs_endpoint, IPC_ENDPOINT_NONE);
        pm_atomic_store_u32(&g_pm.block_endpoint, IPC_ENDPOINT_NONE);
        pm_atomic_store_u32(&g_pm.fb_endpoint, IPC_ENDPOINT_NONE);
        pm_atomic_store_u32(&g_pm.fs_reply_endpoint, IPC_ENDPOINT_NONE);
        pm_atomic_store_u32(&g_pm.fs_ctrl_endpoint, IPC_ENDPOINT_NONE);
        g_pm.fs_request_id = 1;
        g_pm.next_cli_tty = 1;
        g_pm.started = 0;
        g_pm_wait_owner_deny_logged = 0;
        g_pm_kill_owner_deny_logged = 0;
        g_pm_status_owner_deny_logged = 0;
        g_pm_spawn_owner_deny_logged = 0;
        if (boot_info && (boot_info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
            g_pm.module_count = boot_info->module_count;
            g_pm.init_module_index = pm_find_module_index_by_name("sysinit");
        }
        if (list_init(&g_pm.apps,
                      sizeof(pm_app_state_t),
                      (list_impl_t)WASMOS_PM_LIST_IMPL,
                      WASMOS_PM_LIST_ARRAY_CHUNK_CAP) != 0) {
            return -1;
        }
        if (list_init(&g_pm.waits,
                      sizeof(pm_wait_state_t),
                      (list_impl_t)WASMOS_PM_LIST_IMPL,
                      WASMOS_PM_LIST_ARRAY_CHUNK_CAP) != 0) {
            return -1;
        }
        if (list_init(&g_pm.services,
                      sizeof(pm_service_entry_t),
                      (list_impl_t)WASMOS_PM_LIST_IMPL,
                      WASMOS_PM_LIST_ARRAY_CHUNK_CAP) != 0) {
            return -1;
        }
        g_pm.spawn.in_use = 0;
        return 0;
    }

    int handle_kill(uint32_t pm_context_id, const ipc_message_t* msg) {
        uint32_t owner_context = 0;
        process_t* caller = 0;
        process_t* target = 0;

        if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
            return -1;
        }
        caller = process_find_by_context(owner_context);
        if (!caller) {
            if (!g_pm_kill_owner_deny_logged) {
                g_pm_kill_owner_deny_logged = 1;
                klog_write("[test] pm kill owner deny ok\n");
            }
            return -1;
        }

        target = process_get(msg->arg0);
        if (!target || target->parent_pid != caller->pid) {
            return -1;
        }

        if (process_kill(msg->arg0, (int32_t)msg->arg1) != 0) {
            return -1;
        }

        ipc_message_t resp;
        resp.type = PROC_IPC_RESP;
        resp.source = g_pm.proc_endpoint;
        resp.destination = msg->source;
        resp.request_id = msg->request_id;
        resp.arg0 = msg->arg0;
        resp.arg1 = msg->arg1;
        resp.arg2 = 0;
        resp.arg3 = 0;
        return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
    }

    int handle_status(uint32_t pm_context_id, const ipc_message_t* msg) {
        uint32_t owner_context = 0;
        process_t* caller = 0;
        process_t* target = process_get(msg->arg0);
        ipc_message_t resp;

        if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
            return -1;
        }
        caller = process_find_by_context(owner_context);
        if (!caller) {
            if (!g_pm_status_owner_deny_logged) {
                g_pm_status_owner_deny_logged = 1;
                klog_write("[test] pm status owner deny ok\n");
            }
            return -1;
        }

        resp.type = PROC_IPC_RESP;
        resp.source = g_pm.proc_endpoint;
        resp.destination = msg->source;
        resp.request_id = msg->request_id;
        resp.arg0 = msg->arg0;
        resp.arg1 = PROC_STATUS_UNKNOWN;
        resp.arg2 = 0;
        resp.arg3 = 0;

        if (target) {
            if (target->state == PROCESS_STATE_ZOMBIE) {
                resp.arg1 = PROC_STATUS_ZOMBIE;
                resp.arg2 = (uint32_t)target->exit_status;
            } else if (target->state != PROCESS_STATE_UNUSED) {
                resp.arg1 = PROC_STATUS_RUNNING;
            }
        }

        return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
    }

    int handle_wait(uint32_t pm_context_id, const ipc_message_t* msg) {
        uint32_t owner_context = 0;
        process_t* caller = 0;
        process_t* target = 0;
        int32_t exit_status = 0;

        if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
            return -1;
        }
        caller = process_find_by_context(owner_context);
        if (!caller) {
            return -1;
        }

        target = process_get(msg->arg0);
        if (!target || target->parent_pid != caller->pid) {
            return -1;
        }

        if (process_get_exit_status(msg->arg0, &exit_status) == 0) {
            ipc_message_t resp;
            resp.type = PROC_IPC_RESP;
            resp.source = g_pm.proc_endpoint;
            resp.destination = msg->source;
            resp.request_id = msg->request_id;
            resp.arg0 = msg->arg0;
            resp.arg1 = (uint32_t)exit_status;
            resp.arg2 = 0;
            resp.arg3 = 0;
            int rc = ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
            /* Status delivered — reap the zombie now so its process slot is
             * freed (interactive CLI children are never otherwise reaped). */
            process_reap_zombie_pid(msg->arg0);
            return rc;
        }

        pm_wait_state_t* waiter = wait_slot_acquire();
        if (!waiter) {
            return -1;
        }
        waiter->in_use = 1;
        waiter->pid = msg->arg0;
        waiter->reply_endpoint = msg->source;
        waiter->request_id = msg->request_id;
        waiter->owner_context_id = owner_context;
        return 0;
    }

    process_run_result_t entry(process_t* process, void*) {
        ipc_message_t msg;

        if (!process) {
            return PROCESS_RUN_IDLE;
        }

        if (!g_pm.started) {
            uint32_t proc_endpoint = IPC_ENDPOINT_NONE;
            if (ipc_endpoint_create(process->context_id, &proc_endpoint) != IPC_OK) {
                klog_write("[pm] endpoint create failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
            pm_atomic_store_u32(&g_pm.proc_endpoint, proc_endpoint);
            uint32_t fs_reply_endpoint = IPC_ENDPOINT_NONE;
            if (ipc_endpoint_create(process->context_id, &fs_reply_endpoint) != IPC_OK) {
                klog_write("[pm] fs reply endpoint create failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
            pm_atomic_store_u32(&g_pm.fs_reply_endpoint, fs_reply_endpoint);
            uint32_t fs_ctrl_endpoint = IPC_ENDPOINT_NONE;
            if (ipc_endpoint_create(process->context_id, &fs_ctrl_endpoint) != IPC_OK) {
                klog_write("[pm] fs ctrl endpoint create failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
            pm_atomic_store_u32(&g_pm.fs_ctrl_endpoint, fs_ctrl_endpoint);
            uint32_t broker_reply_endpoint = IPC_ENDPOINT_NONE;
            if (ipc_endpoint_create(process->context_id, &broker_reply_endpoint) != IPC_OK) {
                klog_write("[pm] broker reply endpoint create failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
            pm_atomic_store_u32(&g_pm.broker_reply_endpoint, broker_reply_endpoint);

            /* One select set over the three endpoints the run loop drains, so
             * the entry can block instead of busy-polling until any of them has
             * traffic.  broker_reply_endpoint is deliberately NOT in the set:
             * it is drained synchronously inside pm_request_broker_spawn_plan
             * via ipc_recv_blocking_for, never from this loop. */
            uint32_t pm_eps[3] = {proc_endpoint, fs_ctrl_endpoint, fs_reply_endpoint};
            uint32_t pm_select = 0;
            if (ipc_select_listen(process->context_id, pm_eps, 3, &pm_select) != IPC_OK) {
                klog_write("[pm] select setup failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
            pm_atomic_store_u32(&g_pm.select_id, pm_select);
            g_pm.started = 1;
        }

        pm_check_waits(process->context_id);
        pm_reap_apps(process);
        pm_services_class_reap(process->context_id);
        pm_poll_spawn(process->context_id);
        uint32_t fs_ctrl_endpoint = pm_atomic_load_u32(&g_pm.fs_ctrl_endpoint);
        if (fs_ctrl_endpoint != IPC_ENDPOINT_NONE) {
            ipc_message_t ignored;
            while (ipc_recv_for(process->context_id, fs_ctrl_endpoint, &ignored) == IPC_OK) {
            }
        }

        uint32_t proc_endpoint = pm_atomic_load_u32(&g_pm.proc_endpoint);
        int recv_rc = ipc_recv_for(process->context_id, proc_endpoint, &msg);
        if (recv_rc != IPC_OK) {
            /* No request queued.  Block on the select set (proc / fs_ctrl /
             * fs_reply) until any endpoint has traffic, or the poll interval
             * elapses so the exit-driven periodic work above still runs.
             * Returning YIELDED immediately instead would busy-poll and spin the
             * scheduler.  A pending request is drained one-per-dispatch
             * (productive work, not a spin); the sleep happens only once the
             * queue is empty. */
            uint32_t ready_ep = IPC_ENDPOINT_NONE;
            (void)ipc_select_wait(pm_atomic_load_u32(&g_pm.select_id),
                                  process->context_id,
                                  &ready_ep,
                                  WASMOS_PM_POLL_INTERVAL_MS);
            return PROCESS_RUN_YIELDED;
        }

        int rc = -1;
        switch (msg.type) {
        case PROC_IPC_SPAWN:
            rc = pm_handle_spawn(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_CAPS:
            rc = pm_handle_spawn_caps(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_CAPS_V2:
            rc = pm_handle_spawn_caps_v2(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_PATH:
            rc = pm_handle_spawn_path(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_PATH_CAPS:
            rc = pm_handle_spawn_path_caps(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_SYNC:
            rc = pm_handle_spawn_sync(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_CAPS_SYNC:
            rc = pm_handle_spawn_caps_sync(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_PATH_SYNC:
            rc = pm_handle_spawn_path_sync(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_PATH_CAPS_SYNC:
            rc = pm_handle_spawn_path_caps_sync(process->context_id, &msg);
            break;
        case PROC_IPC_NOTIFY_READY:
            rc = pm_handle_notify_ready(process->context_id, &msg);
            break;
        case PROC_IPC_MODULE_META_DESC:
            rc = pm_handle_module_meta_desc(process->context_id, &msg);
            break;
        case PROC_IPC_MODULE_META_PATH:
            rc = pm_handle_module_meta_path(process->context_id, &msg);
            break;
        case PROC_IPC_SUBSYSTEM_REGISTER_BROKER:
            rc = pm_handle_subsystem_register_broker(process->context_id, &msg);
            break;
        case PROC_IPC_EXEC_HANDLER_REGISTER:
            rc = pm_handle_exec_handler_register(process->context_id, &msg);
            break;
        case PROC_IPC_KILL:
            rc = handle_kill(process->context_id, &msg);
            break;
        case PROC_IPC_STATUS:
            rc = handle_status(process->context_id, &msg);
            break;
        case PROC_IPC_WAIT:
            rc = handle_wait(process->context_id, &msg);
            break;
        case SVC_IPC_REGISTER_REQ:
            rc = pm_handle_service_register(process->context_id, &msg);
            break;
        case SVC_IPC_REGISTER_DESC_REQ:
            rc = pm_handle_service_register_desc(process->context_id, &msg);
            break;
        case SVC_IPC_LOOKUP_REQ:
            rc = pm_handle_service_lookup(process->context_id, &msg);
            break;
        case SVC_IPC_LOOKUP_CLASS_REQ:
            rc = pm_handle_service_lookup_class(process->context_id, &msg);
            break;
        case SVC_IPC_SUBSCRIBE_CLASS_REQ:
            rc = pm_handle_class_subscribe(process->context_id, &msg);
            break;
        default:
            /* Unknown/unsolicited type — almost always a stray reply (a *_RESP
             * or *_ERROR) that got misrouted to PM's well-known proc endpoint.
             * Drop it silently.  Never bounce an error back here: a peer that
             * likewise error-replies to unrecognised types (e.g. the RTC
             * driver) would ping-pong with PM forever and peg a CPU. */
            return PROCESS_RUN_YIELDED;
        }

        if (rc != 0) {
            ipc_message_t resp;
            if (msg.type == SVC_IPC_REGISTER_REQ || msg.type == SVC_IPC_REGISTER_DESC_REQ ||
                msg.type == SVC_IPC_LOOKUP_REQ || msg.type == SVC_IPC_LOOKUP_CLASS_REQ ||
                msg.type == SVC_IPC_SUBSCRIBE_CLASS_REQ) {
                resp.type = SVC_IPC_ERROR;
            } else {
                resp.type = PROC_IPC_ERROR;
            }
            resp.source = proc_endpoint;
            resp.destination = msg.source;
            resp.request_id = msg.request_id;
            resp.arg0 = msg.type;
            resp.arg1 = (uint32_t)rc;
            resp.arg2 = 0;
            resp.arg3 = 0;
            ipc_send_from(process->context_id, msg.source, &resp);
        }

        return PROCESS_RUN_YIELDED;
    }
};

static ProcessManager g_process_manager;

/* C entry points onto the single ProcessManager instance.  Everything below runs
 * on PM's own process context: the g_pm fields they touch are unsynchronised and
 * are only safe because PM is single-threaded and (outside a
 * pm_preempt_safe_enter region) not preemptible.  The endpoint accessors are the
 * exception — those go through pm_atomic_load_u32 because other CPUs read them. */

/* Reuses the first free wait slot, or grows the list.  Returns 0 only when the
 * list cannot grow.  The returned slot is NOT yet claimed: the caller sets
 * in_use itself after filling the reply endpoint, request id and owner. */
pm_wait_state_t* pm_wait_slot_acquire(void) {
    return g_process_manager.wait_slot_acquire();
}
/* Next controlling tty for a WANTS_TTY child, cycling 1..3 and advancing the
 * cursor.  Hands out the number unconditionally: nothing checks whether that tty
 * is already claimed, so with more than three live tty children they share. */
uint32_t pm_alloc_cli_tty(void) {
    return g_process_manager.alloc_cli_tty();
}

/* Test seams.  Each fabricates one request that PM will process on a later
 * dispatch, so the deny path under test logs its marker.  All four compile to
 * no-ops unless WASMOS_PM_TEST_HOOKS is set, and all are best-effort: a failure
 * to create the injection endpoint or claim a slot is swallowed, since a seam
 * that cannot fire simply leaves the marker unlogged. */

/* Plants a wait slot whose recorded owner context does not match the reply
 * endpoint's real owner, so the next pm_check_waits() sweep takes the
 * owner-mismatch branch and drops it. */
void process_manager_inject_wait_owner_mismatch_test(uint32_t expected_owner_context_id) {
    ProcessManager::inject_request_t request;
    request.type = 0;
    request.request_id = 0xFFFF0001u;
    request.arg0 = 0;
    request.arg1 = 0;
    request.arg2 = 0;
    request.arg3 = 0;
    request.expected_owner_context_id = expected_owner_context_id;
    request.wait_owner_mismatch = 1;
    g_process_manager.inject(&request);
}
/* Sends a KILL/STATUS/SPAWN request from a freshly created KERNEL-owned endpoint.
 * No process owns that context, so process_find_by_context() fails in each
 * handler and the corresponding owner-deny marker is logged. */
void process_manager_inject_kill_owner_deny_test(void) {
    ProcessManager::inject_request_t request;
    request.type = PROC_IPC_KILL;
    request.request_id = 0xFFFF1001u;
    request.arg0 = 0;
    request.arg1 = 0;
    request.arg2 = 0;
    request.arg3 = 0;
    request.expected_owner_context_id = 0;
    request.wait_owner_mismatch = 0;
    g_process_manager.inject(&request);
}
void process_manager_inject_status_owner_deny_test(void) {
    ProcessManager::inject_request_t request;
    request.type = PROC_IPC_STATUS;
    request.request_id = 0xFFFF1002u;
    request.arg0 = 0;
    request.arg1 = 0;
    request.arg2 = 0;
    request.arg3 = 0;
    request.expected_owner_context_id = 0;
    request.wait_owner_mismatch = 0;
    g_process_manager.inject(&request);
}
void process_manager_inject_spawn_owner_deny_test(void) {
    ProcessManager::inject_request_t request;
    request.type = PROC_IPC_SPAWN;
    request.request_id = 0xFFFF1003u;
    request.arg0 = 0;
    request.arg1 = 0;
    request.arg2 = 0;
    request.arg3 = 0;
    request.expected_owner_context_id = 0;
    request.wait_owner_mismatch = 0;
    g_process_manager.inject(&request);
}

/* Resets PM state and builds its three object lists.  Returns 0, or -1 if any
 * list fails to initialise.  `boot_info` is BORROWED for the life of the system:
 * the pointer is stored and the module table is read back from it on every
 * module-index spawn, so it must reference memory that survives boot.  A NULL
 * boot_info (or one without modules) is accepted and simply leaves the module
 * count at 0 and the sysinit index unset.  Creates no endpoints — those are made
 * lazily on PM's first dispatch, inside its own context. */
int process_manager_init(const boot_info_t* boot_info) {
    return g_process_manager.init(boot_info);
}

/* Well-known endpoint accessors, readable from any CPU.  Each answers
 * IPC_ENDPOINT_NONE until the corresponding service registers (or, for the proc
 * endpoint, until PM's first dispatch creates it), so every caller must handle
 * "not yet" rather than assume availability.  The values are atomics because the
 * writers are PM and service registration while the readers are arbitrary
 * kernel-side code on other CPUs. */
uint32_t process_manager_endpoint(void) {
    return pm_atomic_load_u32(&g_pm.proc_endpoint);
}
uint32_t process_manager_fs_endpoint(void) {
    return pm_atomic_load_u32(&g_pm.fs_endpoint);
}
uint32_t process_manager_block_endpoint(void) {
    return pm_atomic_load_u32(&g_pm.block_endpoint);
}
uint32_t process_manager_framebuffer_endpoint(void) {
    return pm_atomic_load_u32(&g_pm.fb_endpoint);
}

/* Publishes a framebuffer endpoint that has no registering process behind it —
 * the in-kernel framebuffer.  Records it under the name "fb" owned by
 * IPC_CONTEXT_KERNEL, so an ordinary service lookup finds it; a later
 * registration of "fb" by a real process is then refused by pm_service_set,
 * which does not allow an owner change. */
void process_manager_set_framebuffer_endpoint(uint32_t endpoint) {
    pm_atomic_store_u32(&g_pm.fb_endpoint, endpoint);
    (void)pm_service_set("fb", endpoint, IPC_CONTEXT_KERNEL);
}

/* PM's process entry point: one dispatch does the periodic sweeps (wait replies,
 * app reaping, class reaping, sync-spawn poll), then drains at most ONE request
 * from the proc endpoint and dispatches it.  Always returns PROCESS_RUN_YIELDED,
 * except PROCESS_RUN_EXITED if the first-dispatch endpoint setup fails.
 *
 * One request per dispatch is deliberate: it keeps the sweeps running between
 * requests and lets other processes in.  With the queue empty it blocks on the
 * select set for up to WASMOS_PM_POLL_INTERVAL_MS rather than spinning.  A
 * handler returning non-zero is turned into a PROC_IPC_ERROR / SVC_IPC_ERROR
 * reply carrying that code in arg1; a handler returning 0 has already sent its
 * own reply. */
process_run_result_t process_manager_entry(process_t* process, void* arg) {
    return g_process_manager.entry(process, arg);
}

/* Latches a child's readiness from OUTSIDE PM's dispatch loop — the native
 * driver path calls this on its own CPU.  A no-op for an unknown pid.  It
 * deliberately touches nothing but the child's ready flag; see below. */
void process_manager_on_child_ready(uint32_t pid) {
    process_t* proc = process_get(pid);
    if (!proc) {
        return;
    }
    process_notify_ready(proc);
    /* Do NOT read or write g_pm.spawn here.  This function is called from
     * nd_proc_notify_ready() on the native driver's CPU (different from PM's
     * CPU), so any access to g_pm.spawn would be an unsynchronised cross-CPU
     * read/write.  Specifically, PM sets in_use=1 and sync_child_pid in
     * sequence after pm_spawn_from_buffer(); a driver running between those two
     * stores would observe in_use=1 with sync_child_pid still 0, send to
     * endpoint 0 (silent failure), clear in_use=0, and leave PM's
     * pm_poll_sync_spawn with nothing to poll — a permanent hang.
     * pm_poll_sync_spawn already checks child->ready on every PM iteration
     * and sends the PROC_IPC_RESP safely from PM's own context. */
}
