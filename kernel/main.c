#include <stdint.h>
#include "console.h"
#include "pmm.h"
#include "idt.h"
#include "heap.h"

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

    /* Heap de 4 MiB (1024 paginas contiguas) */
    heap_init(1024);
    console_puts("\nHeap: "); console_dec(heap_free() >> 10);
    console_puts(" KiB libres\n");

    char *p1 = kmalloc(100);
    char *p2 = kmalloc(5000);
    char *p3 = kmalloc(32);
    console_puts("kmalloc 100:  "); console_hex((uint64_t)p1);
    console_puts("\nkmalloc 5000: "); console_hex((uint64_t)p2);
    console_puts("\nkmalloc 32:   "); console_hex((uint64_t)p3);

    for (int i = 0; i < 100; i++) p1[i] = 'N';
    p1[99] = 0;

    kfree(p2);
    char *p4 = kmalloc(4000);                /* debe reutilizar el hueco de p2 */
    console_puts("\nfree p2 + kmalloc 4000: "); console_hex((uint64_t)p4);
    console_puts(p4 == p2 ? "  (hueco reutilizado OK)\n" : "  (?)\n");

    kfree(p1); kfree(p3); kfree(p4);         /* todo libre: debe fusionarse */
    console_puts("Tras liberar todo: usado="); console_dec(heap_used());
    console_puts(" B, libre="); console_dec(heap_free() >> 10);
    console_puts(" KiB\n");

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
