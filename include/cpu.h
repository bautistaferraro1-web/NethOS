#ifndef NETHEL_CPU_H
#define NETHEL_CPU_H
#include <stdint.h>

#ifdef HOST_TEST
static inline uint64_t irq_save(void) { return 0; }
static inline void irq_restore(uint64_t f) { (void)f; }
#else
/* Desactiva interrupciones y devuelve el RFLAGS previo */
static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; cli; popq %0" : "=r"(f) : : "memory");
    return f;
}

/* Reactiva interrupciones solo si estaban activas antes */
static inline void irq_restore(uint64_t f)
{
    if (f & 0x200) __asm__ volatile("sti" : : : "memory");
}
#endif

#endif
