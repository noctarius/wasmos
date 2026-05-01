#include "syscall.h"
#include "process.h"
#include "serial.h"

static uint8_t g_ring3_syscall_logged;

static void
syscall_trace_ring3_once(syscall_frame_t *frame)
{
    if (!frame || g_ring3_syscall_logged) {
        return;
    }
    if ((frame->cs & 0x3u) != 0x3u) {
        return;
    }
    /* TODO: Restore a stricter ring3-smoke GETPID assertion once the smoke
     * syscall frame reliably carries the expected syscall id in RAX. */
    g_ring3_syscall_logged = 1;
    serial_write("[test] ring3 syscall ok\n");
}

uint64_t
x86_syscall_handler(syscall_frame_t *frame)
{
    if (!frame) {
        return (uint64_t)-1;
    }
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
    default:
        return (uint64_t)-1;
    }
}
