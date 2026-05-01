#include "syscall.h"
#include "ipc.h"
#include "process.h"
#include "serial.h"
#include "string.h"

static uint8_t g_ring3_syscall_logged;
static uint8_t g_ring3_stress_ok_logged;
static uint32_t g_ring3_getpid_count;
static uint8_t g_ring3_ipc_deny_logged;
static uint8_t g_ring3_ipc_ok_logged;
static uint8_t g_ring3_ipc_call_deny_logged;
static uint8_t g_ring3_ipc_call_ok_logged;
static uint32_t g_syscall_ipc_call_next_request_id = 1;

static int
name_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    return strcmp(a, b) == 0;
}

static void
syscall_trace_ring3_once(syscall_frame_t *frame)
{
    if (!frame || g_ring3_syscall_logged) {
        return;
    }
    if ((frame->cs & 0x3u) != 0x3u) {
        return;
    }
    if ((uint32_t)frame->rax != WASMOS_SYSCALL_GETPID) {
        return;
    }
    process_t *proc = process_get(process_current_pid());
    if (!proc || !name_eq(proc->name, "ring3-smoke")) {
        return;
    }
    g_ring3_syscall_logged = 1;
    serial_write("[test] ring3 syscall ok\n");
}

static void
syscall_trace_ring3_stress(syscall_frame_t *frame)
{
    if (!frame || g_ring3_stress_ok_logged) {
        return;
    }
    if ((frame->cs & 0x3u) != 0x3u) {
        return;
    }
    process_t *proc = process_get(process_current_pid());
    if (!proc || !name_eq(proc->name, "ring3-smoke")) {
        return;
    }
    if ((uint32_t)frame->rax == WASMOS_SYSCALL_GETPID) {
        if (g_ring3_getpid_count < 0xFFFFFFFFu) {
            g_ring3_getpid_count++;
        }
        return;
    }
    if ((uint32_t)frame->rax == WASMOS_SYSCALL_EXIT && g_ring3_getpid_count >= 4096u) {
        g_ring3_stress_ok_logged = 1;
        serial_write("[test] ring3 preempt stress ok\n");
    }
}

uint64_t
x86_syscall_handler(syscall_frame_t *frame)
{
    if (!frame) {
        return (uint64_t)-1;
    }
    syscall_trace_ring3_stress(frame);
    syscall_trace_ring3_once(frame);

    switch ((uint32_t)frame->rax) {
    case WASMOS_SYSCALL_NOP:
        return 0;
    case WASMOS_SYSCALL_GETPID:
        return process_current_pid();
    case WASMOS_SYSCALL_EXIT: {
        process_t *proc = process_get(process_current_pid());
        if (!proc) {
            return (uint64_t)-1;
        }
        process_set_exit_status(proc, (int32_t)frame->rdi);
        process_yield(PROCESS_RUN_EXITED);
        return 0;
    }
    case WASMOS_SYSCALL_YIELD:
        process_yield(PROCESS_RUN_YIELDED);
        return 0;
    case WASMOS_SYSCALL_WAIT: {
        process_t *proc = process_get(process_current_pid());
        uint32_t target_pid = (uint32_t)frame->rdi;
        int32_t exit_status = 0;
        int wait_rc = 0;
        if (!proc) {
            return (uint64_t)-1;
        }
        for (;;) {
            wait_rc = process_wait(proc, target_pid, &exit_status);
            if (wait_rc == 0) {
                return (uint64_t)(int64_t)exit_status;
            }
            if (wait_rc < 0) {
                return (uint64_t)-1;
            }
            process_yield(PROCESS_RUN_BLOCKED);
        }
    }
    case WASMOS_SYSCALL_IPC_NOTIFY: {
        process_t *proc = process_get(process_current_pid());
        uint32_t endpoint = (uint32_t)frame->rdi;
        int rc = IPC_ERR_INVALID;
        if (!proc) {
            return (uint64_t)-1;
        }
        rc = ipc_notify_from(proc->context_id, endpoint);
        if (name_eq(proc->name, "ring3-smoke")) {
            if (!g_ring3_ipc_deny_logged && endpoint == 0xFFFFFFFFu && rc == IPC_ERR_INVALID) {
                g_ring3_ipc_deny_logged = 1;
                serial_write("[test] ring3 ipc syscall deny ok\n");
            }
            if (!g_ring3_ipc_ok_logged && rc == IPC_OK) {
                g_ring3_ipc_ok_logged = 1;
                serial_write("[test] ring3 ipc syscall ok\n");
            }
        }
        return (uint64_t)(int64_t)rc;
    }
    case WASMOS_SYSCALL_IPC_CALL: {
        process_t *proc = process_get(process_current_pid());
        uint32_t endpoint = (uint32_t)frame->rdi;
        uint32_t request_id = g_syscall_ipc_call_next_request_id++;
        int rc = IPC_ERR_INVALID;
        ipc_message_t req;
        ipc_message_t resp;
        if (!proc) {
            return (uint64_t)-1;
        }
        if (request_id == 0) {
            request_id = g_syscall_ipc_call_next_request_id++;
        }
        req.type = (uint32_t)frame->rsi;
        req.source = endpoint;
        req.destination = endpoint;
        req.request_id = request_id;
        req.arg0 = (uint32_t)frame->rdx;
        req.arg1 = (uint32_t)frame->rcx;
        req.arg2 = (uint32_t)frame->r8;
        req.arg3 = (uint32_t)frame->r9;
        rc = ipc_send_from(proc->context_id, endpoint, &req);
        if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_call_deny_logged &&
            endpoint == 0xFFFFFFFFu && rc == IPC_ERR_INVALID) {
            g_ring3_ipc_call_deny_logged = 1;
            serial_write("[test] ring3 ipc call deny ok\n");
        }
        if (rc != IPC_OK) {
            return (uint64_t)(int64_t)rc;
        }
        for (;;) {
            rc = ipc_recv_for(proc->context_id, endpoint, &resp);
            if (rc == IPC_EMPTY) {
                process_yield(PROCESS_RUN_BLOCKED);
                continue;
            }
            if (rc != IPC_OK) {
                return (uint64_t)(int64_t)rc;
            }
            if (resp.request_id != request_id) {
                continue;
            }
            frame->rdx = (uint64_t)resp.arg0;
            if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_call_ok_logged &&
                endpoint != 0xFFFFFFFFu && (uint32_t)frame->rdx == req.arg0) {
                g_ring3_ipc_call_ok_logged = 1;
                serial_write("[test] ring3 ipc call ok\n");
            }
            return 0;
        }
    }
    default:
        return (uint64_t)-1;
    }
}
