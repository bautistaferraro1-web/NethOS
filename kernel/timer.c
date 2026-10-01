#include "timer.h"
#include "sched.h"
#include "irq.h"
#include "io.h"

#define PIT_FREQ 1193182u

static volatile uint64_t ticks;
static uint32_t timer_hz;

static void timer_irq(void) { ticks++; sched_tick(); }

void timer_init(uint32_t hz)
{
    timer_hz = hz;
    uint32_t div = PIT_FREQ / hz;
    outb(0x43, 0x36);                        /* canal 0, lo/hi, modo 3 */
    outb(0x40, div & 0xFF);
    outb(0x40, (div >> 8) & 0xFF);
    irq_register(0, timer_irq);
    pic_unmask(0);
}

uint64_t timer_ticks(void) { return ticks; }

/* Duerme la tarea actual; requiere interrupciones activas */
void timer_sleep(uint32_t ms)
{
    uint64_t end = ticks + ((uint64_t)ms * timer_hz) / 1000;
    while (ticks < end) {
        yield();
        __asm__ volatile("hlt");
    }
}
