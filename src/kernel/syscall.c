#include "syscall.h"
#include "process.h"
#include "serial.h"
#include "string.h"

static uint8_t g_ring3_syscall_logged;

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

static uint64_t
syscall_finish_with_resched(syscall_frame_t *frame, uint64_t result)
{
    if (!frame) {
        return result;
    }
    if ((frame->cs & 0x3u) != 0x3u) {
        return result;
    }
    if (!preempt_is_enabled() || !process_should_resched()) {
        return result;
    }
    process_clear_resched();
    process_yield(PROCESS_RUN_YIELDED);
    return result;
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
        return syscall_finish_with_resched(frame, 0);
    case WASMOS_SYSCALL_GETPID:
        return syscall_finish_with_resched(frame, process_current_pid());
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
        return syscall_finish_with_resched(frame, (uint64_t)-1);
    }
}
