/* switch_context(uint64_t *old_rsp, uint64_t new_rsp)
 * Guarda los registros callee-saved en el stack actual, cambia de stack y los restaura. */
.section .text
.code64

.global switch_context
switch_context:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov  %rsp, (%rdi)
    mov  %rsi, %rsp
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %rbp
    pop  %rbx
    ret

/* Primera ejecucion de una tarea: rbx = funcion, r12 = argumento */
.global task_trampoline
task_trampoline:
#ifndef HOST_TEST
    sti
#endif
    mov  %r12, %rdi
    call *%rbx
    call task_exit
    hlt

.section .note.GNU-stack,"",@progbits
