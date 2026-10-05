/* Nethel - boot.S : Multiboot2 -> long mode -> mitad alta -> kernel_main(magic, mb2_info)
   Todo lo que esta en .boot.* vive en fisico (1 MiB) y corre con la identidad.
   El resto del kernel esta enlazado en KERNEL_VMA + fisico. */

.set MB2_MAGIC,  0xE85250D6
.set KERNEL_VMA, 0xFFFFFFFF80000000

.section .multiboot2, "a"
.align 8
mb2_header_start:
    .long MB2_MAGIC
    .long 0
    .long mb2_header_end - mb2_header_start
    .long -(MB2_MAGIC + 0 + (mb2_header_end - mb2_header_start))
    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

.section .boot.bss, "aw", @nobits
.align 4096
pml4:    .skip 4096
pdpt:    .skip 4096
pdpt_hi: .skip 4096
pd:      .skip 4096
.align 16
stack_bottom: .skip 16384
stack_top:
saved_magic: .skip 4
saved_mb2:   .skip 4

.section .boot.data, "a"
.align 8
gdt:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF
    .quad 0x00CF92000000FFFF
gdt_end:
gdt_ptr:
    .word gdt_end - gdt - 1
    .long gdt

.section .boot.text, "ax"
.code32
.global _start
_start:
    cli
    mov  $stack_top, %esp
    mov  %eax, saved_magic
    mov  %ebx, saved_mb2

    mov  $pdpt, %eax
    or   $0x3, %eax
    mov  %eax, pml4                 /* pml4[0]:   identidad                    */
    mov  %eax, pml4+2048            /* pml4[256]: direct map alto              */
    mov  $pdpt_hi, %eax
    or   $0x3, %eax
    mov  %eax, pml4+4088            /* pml4[511]: kernel en la mitad alta      */
    mov  $pd, %eax
    or   $0x3, %eax
    mov  %eax, pdpt                 /* pdpt[0]                                 */
    mov  %eax, pdpt_hi+4080         /* pdpt_hi[510] = 0xFFFFFFFF80000000       */

    xor  %ecx, %ecx
1:
    mov  %ecx, %eax
    shl  $21, %eax
    or   $0x83, %eax
    mov  %eax, pd(,%ecx,8)
    movl $0, pd+4(,%ecx,8)
    inc  %ecx
    cmp  $512, %ecx
    jne  1b

    mov  $pml4, %eax
    mov  %eax, %cr3

    mov  %cr4, %eax
    or   $0x20, %eax
    mov  %eax, %cr4

    mov  $0xC0000080, %ecx
    rdmsr
    or   $0x100, %eax
    wrmsr

    mov  %cr0, %eax
    or   $0x80000000, %eax
    mov  %eax, %cr0

    lgdt gdt_ptr
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    xor  %ax, %ax
    mov  %ax, %fs
    mov  %ax, %gs

    mov  saved_magic(%rip), %edi            /* rip-relative: solo desde codigo bajo */
    mov  saved_mb2(%rip), %esi
    movabs $(stack_top + KERNEL_VMA), %rsp  /* stack por su alias alto */
    movabs $higher_half, %rax
    jmp  *%rax

.section .text
.code64
higher_half:
    call kernel_main
hang:
    cli
    hlt
    jmp  hang

.section .note.GNU-stack,"",@progbits
