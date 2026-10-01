ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot2 : { *(.multiboot2) }
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(COMMON) *(.bss*) }

    kernel_end = .;
}
