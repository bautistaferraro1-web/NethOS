#include <stdint.h>
#include "console.h"
#include "pmm.h"

#define MB2_MAGIC 0x36D76289

void kernel_main(uint64_t magic, uint64_t mb2_info)
{
    console_clear();
    console_puts("Nethel kernel 0.1 - NethOS\n\n");

    if ((uint32_t)magic != MB2_MAGIC) {
        console_puts("ERROR: no arrancamos por Multiboot2\n");
        for (;;) __asm__ volatile("hlt");
    }

    pmm_init(mb2_info);

    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    console_puts("\nalloc_page: "); console_hex(a);
    console_puts("\nalloc_page: "); console_hex(b);
    pmm_free_page(a);
    uint64_t c = pmm_alloc_page();
    console_puts("\nfree+alloc: "); console_hex(c);
    console_puts(c == a ? "  (reutilizada OK)\n" : "  (?)\n");

    for (;;) __asm__ volatile("hlt");
}
