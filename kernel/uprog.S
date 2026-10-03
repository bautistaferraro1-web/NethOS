.section .rodata
.code64
.global uprog_start
.global uprog_end

uprog_start:
    mov  $1, %eax                  /* write(1, msg1, len) */
    mov  $1, %edi
    lea  msg1(%rip), %rsi
    mov  $(msg1_end - msg1), %edx
    int  $0x80

    mov  $24, %eax                 /* yield() */
    int  $0x80

    mov  $1, %eax                  /* write(1, msg2, len) */
    mov  $1, %edi
    lea  msg2(%rip), %rsi
    mov  $(msg2_end - msg2), %edx
    int  $0x80

    mov  $60, %eax                 /* exit(0) */
    xor  %edi, %edi
    int  $0x80
1:  jmp  1b

msg1: .ascii "[user] Hola desde ring 3!\n"
msg1_end:
msg2: .ascii "[user] write() y yield() OK\n"
msg2_end:
uprog_end:

.section .note.GNU-stack,"",@progbits
