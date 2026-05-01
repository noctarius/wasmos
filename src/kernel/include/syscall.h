#ifndef WASMOS_SYSCALL_H
#define WASMOS_SYSCALL_H

#include <stdint.h>

typedef enum {
    WASMOS_SYSCALL_NOP = 0,
    WASMOS_SYSCALL_GETPID = 1,
    WASMOS_SYSCALL_EXIT = 2,
    WASMOS_SYSCALL_YIELD = 3,
    WASMOS_SYSCALL_WAIT = 4
} wasmos_syscall_id_t;

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

#endif
