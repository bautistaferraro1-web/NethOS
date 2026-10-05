#ifndef NETHEL_FPU_H
#define NETHEL_FPU_H
#include <stdint.h>

/* Area de fxsave/fxrstor: 512 bytes, alineada a 16 */
struct fpu_state { uint8_t d[512]; } __attribute__((aligned(16)));

#ifdef HOST_TEST
static inline void fpu_init(void) {}
static inline void fpu_init_state(struct fpu_state *s) { (void)s; }
static inline void fpu_save(struct fpu_state *s) { (void)s; }
static inline void fpu_restore(const struct fpu_state *s) { (void)s; }
#else
void fpu_init(void);                          /* habilita SSE y captura el estado limpio */
void fpu_init_state(struct fpu_state *s);     /* copia el estado limpio */
static inline void fpu_save(struct fpu_state *s)
{ __asm__ volatile("fxsave %0" : "=m"(*s) : : "memory"); }
static inline void fpu_restore(const struct fpu_state *s)
{ __asm__ volatile("fxrstor %0" : : "m"(*s) : "memory"); }
#endif

#endif
