#include <stdint.h>
#include "console.h"
#include "pmm.h"
#include "idt.h"
#include "heap.h"
#include "vmm.h"
#include "irq.h"
#include "timer.h"
#include "keyboard.h"
#include "sched.h"

#define MB2_MAGIC 0x36D76289
#define HZ        100

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int put_str(char *b, int i, const char *s)
{
    while (*s) b[i++] = *s++;
    return i;
}

static int put_num(char *b, int i, uint64_t v)
{
    char t[21];
    int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = '0' + (v % 10); v /= 10; }
    while (n) b[i++] = t[--n];
    return i;
}

static void status(uint64_t secs)
{
    char b[32];
    int i = 0;
    i = put_str(b, i, " uptime: ");
    i = put_num(b, i, secs);
    i = put_str(b, i, "s ");
    b[i] = 0;
    console_status(b);
}

/* Tarea que mantiene la barra de estado */
static void status_task(void *arg)
{
    (void)arg;
    for (;;) {
        status(timer_ticks() / HZ);
        timer_sleep(250);
    }
}

/* Tarea de demo: cuenta 5 segundos y termina */
static void counter_task(void *arg)
{
    uint64_t n = (uint64_t)arg;
    for (int k = 1; k <= 5; k++) {
        char b[48];
        int i = 0;
        i = put_str(b, i, "[tarea ");
        i = put_num(b, i, n);
        i = put_str(b, i, "] paso ");
        i = put_num(b, i, k);
        i = put_str(b, i, "/5\n");
        b[i] = 0;
        console_puts(b);
        timer_sleep(1000);
    }
}

static uint64_t spawn_n;

static void run_command(const char *cmd)
{
    if (cmd[0] == 0) return;
    if (streq(cmd, "help")) {
        console_puts("Comandos: help  mem  uptime  ps  spawn  clear\n");
    } else if (streq(cmd, "mem")) {
        console_puts("PMM libre: "); console_dec((pmm_free_pages() * PAGE_SIZE) >> 20);
        console_puts(" MiB | Heap: usado "); console_dec(heap_used());
        console_puts(" B, libre "); console_dec(heap_free() >> 10);
        console_puts(" KiB\n");
    } else if (streq(cmd, "uptime")) {
        console_puts("Ticks: "); console_dec(timer_ticks());
        console_puts(" ("); console_dec(timer_ticks() / HZ); console_puts(" s)\n");
    } else if (streq(cmd, "ps")) {
        sched_list();
    } else if (streq(cmd, "spawn")) {
        int id = task_create("contador", counter_task, (void *)++spawn_n);
        if (id < 0) console_puts("no se pudo crear la tarea\n");
        else { console_puts("tarea creada, id "); console_dec((uint64_t)id); console_putc('\n'); }
    } else if (streq(cmd, "clear")) {
        console_clear();
    } else {
        console_puts("comando desconocido: "); console_puts(cmd);
        console_puts("  (proba 'help')\n");
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

    sched_init();
    pic_init();
    timer_init(HZ);
    keyboard_init();
    __asm__ volatile("sti");
    console_puts("PIC + timer (100 Hz) + teclado: OK\n");

    task_create("status", status_task, 0);
    console_puts("Scheduler: OK (round-robin, 100 ms)\n");

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

    for (;;) {
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
