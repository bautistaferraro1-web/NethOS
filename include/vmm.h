#ifndef NETHEL_VMM_H
#define NETHEL_VMM_H
#include <stdint.h>

#define VMM_PRESENT 0x001ULL
#define VMM_WRITE   0x002ULL
#define VMM_USER    0x004ULL
#define VMM_HUGE    0x080ULL

void vmm_init(void);
int  vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);   /* 0 = ok, -1 = error */
int  vmm_unmap(uint64_t virt);
int  vmm_translate(uint64_t virt, uint64_t *phys);            /* 0 = mapeada */
void vmm_selftest(void);

#endif
