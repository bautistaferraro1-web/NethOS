ENTRY(_start)

__kvma = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 1M;

    /* Arranque: VMA = LMA (fisico). GRUB salta aca. */
    .boot : {
        *(.multiboot2)
        *(.boot.text)
        *(.boot.data)
    }
    .boot.bss : { *(.boot.bss) }

    /* Kernel: VMA alta, LMA = VMA - __kvma (sigue pegado en fisico) */
    . = ALIGN(4096);
    . += __kvma;

    .text   : AT(ADDR(.text)   - __kvma) { *(.text*) }
    .rodata : AT(ADDR(.rodata) - __kvma) { *(.rodata*) }
    .data   : AT(ADDR(.data)   - __kvma) { *(.data*) }
    .bss    : AT(ADDR(.bss)    - __kvma) { *(COMMON) *(.bss*) }

    kernel_end = .;
}
