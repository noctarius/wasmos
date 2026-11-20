/* stubs_process_platform.h - what stubs_process_platform.c records for a
 * lifecycle test to assert on.
 *
 * The rest of that file is inert by design; these are the exceptions, each a
 * contract process.c owes to a neighbour it does not own:
 *
 *   - g_stub_exit_on_dispatch_pid: a pid whose non-worker dispatches report
 *     PROCESS_RUN_EXITED, so a case can end a process through the dispatcher's
 *     own exit branch. 0 matches nothing.
 *   - the owed-message log: every ipc_owed_post / ipc_owed_cancel process.c
 *     makes, in order. A record is an opaque token the test made up; the stub
 *     never dereferences or frees it.
 */
#ifndef WASMOS_TEST_STUBS_PROCESS_PLATFORM_H
#define WASMOS_TEST_STUBS_PROCESS_PLATFORM_H

#include <stdint.h>

#define STUB_OWED_LOG_MAX 32u

typedef struct {
    const void* owed;
    uint32_t posted; /* 1 = ipc_owed_post, 0 = ipc_owed_cancel */
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
} stub_owed_event_t;

extern uint32_t g_stub_exit_on_dispatch_pid;
extern stub_owed_event_t g_stub_owed_log[STUB_OWED_LOG_MAX];
extern uint32_t g_stub_owed_count;

#endif
