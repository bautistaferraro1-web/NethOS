#ifndef NETHEL_IRQ_H
#define NETHEL_IRQ_H
#include <stdint.h>

typedef void (*irq_handler_t)(void);

void pic_init(void);                 /* remapea el PIC a vectores 32-47, todo enmascarado */
void pic_unmask(int irq);
void irq_register(int irq, irq_handler_t h);
void irq_dispatch(int irq);          /* llamado desde exception_handler */

#endif
