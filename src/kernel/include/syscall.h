#ifndef WASMOS_SYSCALL_H
#define WASMOS_SYSCALL_H

#include <stdint.h>

typedef enum {
    WASMOS_SYSCALL_NOP = 0,
    WASMOS_SYSCALL_GETPID = 1,
    WASMOS_SYSCALL_EXIT = 2,
    WASMOS_SYSCALL_YIELD = 3,
    WASMOS_SYSCALL_WAIT = 4,
    WASMOS_SYSCALL_IPC_NOTIFY = 5,
    WASMOS_SYSCALL_IPC_CALL = 6
} wasmos_syscall_id_t;

/* int 0x80 syscall ABI (current minimal contract):
 * - syscall id: RAX (see wasmos_syscall_id_t)
 * - args: RDI, RSI, RDX, RCX, R8, R9 (syscall-specific)
 * - return:
 *   - RAX: primary return value (0 or positive success value, negative error)
 *   - RDX: optional secondary value for select calls (ipc_call reply arg0)
 *
 * Current syscall argument/return contract:
 * - NOP:            RAX=0
 * - GETPID:         RAX=pid
 * - EXIT:           RDI=exit_status; does not return to caller path
 * - YIELD:          RAX=0
 * - WAIT:           RDI=child_pid; RAX=child_exit_status or -1
 * - IPC_NOTIFY:     RDI=endpoint; RAX=ipc_result_t
 * - IPC_CALL:       RDI=endpoint, RSI=type, RDX/RCX/R8/R9=arg0..arg3
 *                   RAX=ipc_result_t (0 on success), RDX=reply arg0 on success
 *                   RDX=0 on error
 *                   current behavior blocks by yielding until a reply with a
 *                   matching request_id is received
 *                   current reply ABI returns arg0 only (arg1..arg3 ignored)
 */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
} syscall_frame_t;

uint64_t x86_syscall_handler(syscall_frame_t *frame);
void syscall_set_ipc_call_echo_endpoint(uint32_t endpoint);
uint32_t syscall_ipc_call_echo_endpoint(void);

#endif
