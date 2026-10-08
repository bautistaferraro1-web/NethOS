.section .text
.code64

.macro ISR_NOERR n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro ISR_ERR n
isr\n:
    pushq $\n
    jmp isr_common
.endm

.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
.if (\n == 8) || (\n >= 10 && \n <= 14) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
    ISR_ERR \n
.else
    ISR_NOERR \n
.endif
.endr

isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov  %rsp, %rdi
    cld
    call exception_handler
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rbp
    pop  %rdi
    pop  %rsi
    pop  %rdx
    pop  %rcx
    pop  %rbx
    pop  %rax
    add  $16, %rsp
    iretq

.section .rodata
.align 8
.global isr_table
isr_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .quad isr\n
.endr

.section .note.GNU-stack,"",@progbits

.section .text
.code64
.global isr128
isr128:
    pushq $0
    pushq $128
    jmp isr_common

/* Entrada por la instruccion `syscall` (MSR LSTAR). Entra con IF=0, rcx=rip, r11=rflags,
   rsp todavia de usuario. Arma el mismo frame que int 0x80 y reusa isr128/isr_common. */
.section .text
.code64
.global syscall_entry
syscall_entry:
    mov  %rsp, syscall_ustack(%rip)
    mov  syscall_kstack(%rip), %rsp
    pushq $0x1B                        /* ss  = SEL_UDATA */
    pushq syscall_ustack(%rip)         /* rsp de usuario */
    pushq %r11                         /* rflags */
    pushq $0x23                        /* cs  = SEL_UCODE */
    pushq %rcx                         /* rip */
    jmp  isr128
