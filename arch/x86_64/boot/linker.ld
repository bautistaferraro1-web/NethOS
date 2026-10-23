ENTRY(_start)

__kvma = 0xFFFFFFFF80000000;

PHDRS
{
    boot_rx PT_LOAD FLAGS(5);
    boot_rw PT_LOAD FLAGS(6);
    text    PT_LOAD FLAGS(5);
    rodata  PT_LOAD FLAGS(4);
    data    PT_LOAD FLAGS(6);
    stack   PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    /* Arranque: VMA = LMA, cargado por GRUB en 1 MiB. */
    . = 1M;

    .boot :
    {
        *(.multiboot2)
        *(.boot.text)
        *(.boot.data)
    } :boot_rx

    /* Tablas de paginas, stack y datos modificables del arranque. */
    . = ALIGN(4096);
    .boot.bss :
    {
        *(.boot.bss)
    } :boot_rw

    /* Kernel enlazado en la mitad alta, cargado en memoria física. */
    . = ALIGN(4096);
    . += __kvma;

    .text : AT(ADDR(.text) - __kvma)
    {
        *(.text*)
    } :text

    /* Separamos las paginas de solo lectura del código y de los datos. */
    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - __kvma)
    {
        *(.rodata*)
    } :rodata

    .eh_frame : AT(ADDR(.eh_frame) - __kvma)
    {
        *(.eh_frame*)
    } :rodata

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - __kvma)
    {
        *(.data*)
    } :data

    .bss : AT(ADDR(.bss) - __kvma)
    {
        *(COMMON)
        *(.bss*)
    } :data

    kernel_end = .;
}
