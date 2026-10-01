#include "irq.h"
#include "io.h"

#define PIC1      0x20
#define PIC1_DATA 0x21
#define PIC2      0xA0
#define PIC2_DATA 0xA1
#define EOI       0x20

static irq_handler_t handlers[16];

void pic_init(void)
{
    outb(PIC1, 0x11); io_wait();             /* ICW1: init + ICW4 */
    outb(PIC2, 0x11); io_wait();
    outb(PIC1_DATA, 32); io_wait();          /* ICW2: offsets (IRQ0-7 -> 32-39) */
    outb(PIC2_DATA, 40); io_wait();          /*                IRQ8-15 -> 40-47 */
    outb(PIC1_DATA, 4);  io_wait();          /* ICW3: esclavo en IRQ2 */
    outb(PIC2_DATA, 2);  io_wait();
    outb(PIC1_DATA, 1);  io_wait();          /* ICW4: modo 8086 */
    outb(PIC2_DATA, 1);  io_wait();

    outb(PIC1_DATA, 0xFF);                   /* todo enmascarado */
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(int irq)
{
    if (irq < 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << irq));
    } else {
        outb(PIC2_DATA, inb(PIC2_DATA) & ~(1 << (irq - 8)));
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 2));   /* cascada */
    }
}

void irq_register(int irq, irq_handler_t h)
{
    if (irq >= 0 && irq < 16) handlers[irq] = h;
}

void irq_dispatch(int irq)
{
    /* EOI primero: el handler del timer puede cambiar de tarea y no volver enseguida */
    if (irq >= 8) outb(PIC2, EOI);
    outb(PIC1, EOI);
    if (handlers[irq]) handlers[irq]();
}
