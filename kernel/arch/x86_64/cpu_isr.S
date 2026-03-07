.section .text

.extern x86_exception_panic

.macro DECL_EXC num
.global isr_exception_\num
isr_exception_\num:
    mov $\num, %rdi
    call x86_exception_panic
1:
    cli
    hlt
    jmp 1b
.endm

DECL_EXC 0
DECL_EXC 1
DECL_EXC 2
DECL_EXC 3
DECL_EXC 4
DECL_EXC 5
DECL_EXC 6
DECL_EXC 7
DECL_EXC 8
DECL_EXC 9
DECL_EXC 10
DECL_EXC 11
DECL_EXC 12
DECL_EXC 13
DECL_EXC 14
DECL_EXC 15
DECL_EXC 16
DECL_EXC 17
DECL_EXC 18
DECL_EXC 19
DECL_EXC 20
DECL_EXC 21
DECL_EXC 22
DECL_EXC 23
DECL_EXC 24
DECL_EXC 25
DECL_EXC 26
DECL_EXC 27
DECL_EXC 28
DECL_EXC 29
DECL_EXC 30
DECL_EXC 31

.global x86_exception_stub_table
x86_exception_stub_table:
    .quad isr_exception_0
    .quad isr_exception_1
    .quad isr_exception_2
    .quad isr_exception_3
    .quad isr_exception_4
    .quad isr_exception_5
    .quad isr_exception_6
    .quad isr_exception_7
    .quad isr_exception_8
    .quad isr_exception_9
    .quad isr_exception_10
    .quad isr_exception_11
    .quad isr_exception_12
    .quad isr_exception_13
    .quad isr_exception_14
    .quad isr_exception_15
    .quad isr_exception_16
    .quad isr_exception_17
    .quad isr_exception_18
    .quad isr_exception_19
    .quad isr_exception_20
    .quad isr_exception_21
    .quad isr_exception_22
    .quad isr_exception_23
    .quad isr_exception_24
    .quad isr_exception_25
    .quad isr_exception_26
    .quad isr_exception_27
    .quad isr_exception_28
    .quad isr_exception_29
    .quad isr_exception_30
    .quad isr_exception_31
