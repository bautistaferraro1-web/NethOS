#include <stdint.h>
#include "console.h"
#include "pmm.h"
#include "idt.h"
#include "heap.h"
#include "vmm.h"
#include "irq.h"
#include "timer.h"
#include "keyboard.h"

#define MB2_MAGIC 0x36D76289
#define HZ        100

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void status(uint64_t secs)
{
    char buf[32];
    int i = 0;
    const char *pre = " uptime: ";
    while (*pre) buf[i++] = *pre++;
    char tmp[21];
    int t = 0;
    if (!secs) tmp[t++] = '0';
    while (secs) { tmp[t++] = '0' + (secs % 10); secs /= 10; }
    while (t) buf[i++] = tmp[--t];
    buf[i++] = 's';
    buf[i++] = ' ';
    buf[i] = 0;
    console_status(buf);
}

static void run_command(const char *cmd)
{
    if (cmd[0] == 0) return;
    if (streq(cmd, "help")) {
        console_puts("Comandos: help  mem  uptime  clear\n");
    } else if (streq(cmd, "mem")) {
        console_puts("PMM libre: "); console_dec((pmm_free_pages() * PAGE_SIZE) >> 20);
        console_puts(" MiB | Heap: usado "); console_dec(heap_used());
        console_puts(" B, libre "); console_dec(heap_free() >> 10);
        console_puts(" KiB\n");
    } else if (streq(cmd, "uptime")) {
        console_puts("Ticks: "); console_dec(timer_ticks());
        console_puts(" ("); console_dec(timer_ticks() / HZ); console_puts(" s)\n");
    } else if (streq(cmd, "clear")) {
        console_clear();
    } else {
        console_puts("comando desconocido: "); console_puts(cmd);
        console_puts("  (probá 'help')\n");
    }
}

void kernel_main(uint64_t magic, uint64_t mb2_info)
{
    console_clear();
    console_puts("Nethel kernel 0.1 - NethOS\n\n");

    if ((uint32_t)magic != MB2_MAGIC) {
        console_puts("ERROR: no arrancamos por Multiboot2\n");
        for (;;) __asm__ volatile("hlt");
    }

    idt_init();
    console_puts("IDT: OK (excepciones + IRQ 0-47)\n\n");

    pmm_init(mb2_info);
    vmm_init();
    vmm_selftest();
    heap_init(1024);                         /* 4 MiB */
    console_puts("\nHeap: "); console_dec(heap_free() >> 10);
    console_puts(" KiB libres\n");

    pic_init();
    timer_init(HZ);
    keyboard_init();
    __asm__ volatile("sti");
    console_puts("PIC + timer (100 Hz) + teclado: OK\n");

#ifdef TEST_PF
    console_puts("\nProvocando page fault en 0x200000000...\n");
    *(volatile uint64_t *)0x200000000ULL = 1;
#endif
#ifdef TEST_DE
    console_puts("\nProvocando division por cero...\n");
    volatile int z = 0;
    volatile int r = 1 / z;
    (void)r;
#endif

    console_puts("\nNethel listo. Escribi 'help'.\n> ");

    char line[80];
    int len = 0;
    uint64_t last_sec = (uint64_t)-1;

    for (;;) {
        uint64_t sec = timer_ticks() / HZ;
        if (sec != last_sec) { last_sec = sec; status(sec); }

        int c;
        while ((c = keyboard_getc()) >= 0) {
            if (c == '\n') {
                console_putc('\n');
                line[len] = 0;
                run_command(line);
                len = 0;
                console_puts("> ");
            } else if (c == '\b') {
                if (len > 0) { len--; console_putc('\b'); }
            } else if (c >= 32 && len < (int)sizeof(line) - 1) {
                line[len++] = (char)c;
                console_putc((char)c);
            }
        }
        __asm__ volatile("hlt");             /* dormir hasta la proxima interrupcion */
    }
}
