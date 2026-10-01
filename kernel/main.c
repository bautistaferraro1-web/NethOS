#include <stdint.h>
#include "console.h"
#include "pmm.h"
#include "idt.h"

#define MB2_MAGIC 0x36D76289

void kernel_main(uint64_t magic, uint64_t mb2_info)
{
    console_clear();
    console_puts("Nethel kernel 0.1 - NethOS\n\n");

    if ((uint32_t)magic != MB2_MAGIC) {
        console_puts("ERROR: no arrancamos por Multiboot2\n");
        for (;;) __asm__ volatile("hlt");
    }

    idt_init();
    console_puts("IDT: OK (excepciones 0-31)\n\n");

    pmm_init(mb2_info);

    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    console_puts("\nalloc_page: "); console_hex(a);
    console_puts("\nalloc_page: "); console_hex(b);
    pmm_free_page(a);
    uint64_t c = pmm_alloc_page();
    console_puts("\nfree+alloc: "); console_hex(c);
    console_puts(c == a ? "  (reutilizada OK)\n" : "  (?)\n");

#ifdef TEST_PF
    console_puts("\nProvocando page fault en 0x40000000...\n");
    *(volatile uint64_t *)0x40000000ULL = 1;
#endif
#ifdef TEST_DE
    console_puts("\nProvocando division por cero...\n");
    volatile int z = 0;
    volatile int r = 1 / z;
    (void)r;
#endif

    for (;;) __asm__ volatile("hlt");
}
