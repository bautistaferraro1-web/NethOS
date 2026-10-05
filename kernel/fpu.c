#include "fpu.h"

static struct fpu_state fpu_default;

void fpu_init(void)
{
    uint64_t cr0, cr4;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);                      /* EM = 0: hay FPU/SSE */
    cr0 &= ~(1ULL << 3);                      /* TS = 0 */
    cr0 |=  (1ULL << 1);                      /* MP = 1 */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);        /* OSFXSR | OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    __asm__ volatile("fninit");
    fpu_save(&fpu_default);                   /* FCW=0x37F, MXCSR=0x1F80 */
}

void fpu_init_state(struct fpu_state *s)
{
    const uint64_t *a = (const uint64_t *)&fpu_default;
    uint64_t *b = (uint64_t *)s;
    for (int i = 0; i < 64; i++) b[i] = a[i];
}
