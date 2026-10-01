#include "timer.h"
#include "irq.h"
#include "io.h"

#define PIT_FREQ 1193182u

static volatile uint64_t ticks;

static void timer_irq(void) { ticks++; }

void timer_init(uint32_t hz)
{
    uint32_t div = PIT_FREQ / hz;
    outb(0x43, 0x36);                        /* canal 0, lo/hi, modo 3 */
    outb(0x40, div & 0xFF);
    outb(0x40, (div >> 8) & 0xFF);
    irq_register(0, timer_irq);
    pic_unmask(0);
}

uint64_t timer_ticks(void) { return ticks; }
