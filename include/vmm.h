#ifndef NETHEL_VMM_H
#define NETHEL_VMM_H
#include <stdint.h>

#define VMM_PRESENT 0x001ULL
#define VMM_WRITE   0x002ULL
#define VMM_USER    0x004ULL
#define VMM_HUGE    0x080ULL

void vmm_init(void);
int  vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);   /* espacio del kernel; 0 = ok, -1 = error */
int  vmm_unmap(uint64_t virt);
int  vmm_translate(uint64_t virt, uint64_t *phys);            /* espacio activo; 0 = mapeada */
void vmm_selftest(void);

/* Espacios de direcciones: un "espacio" es la direccion fisica de su PML4 */
uint64_t vmm_kernel_space(void);
uint64_t vmm_create_space(void);                              /* 0 si no hay memoria */
void     vmm_destroy_space(uint64_t space);                   /* libera tablas y paginas de usuario */
void     vmm_switch(uint64_t space);                          /* carga CR3 si cambia */
int      vmm_map_in(uint64_t space, uint64_t virt, uint64_t phys, uint64_t flags);
int      vmm_unmap_in(uint64_t space, uint64_t virt);                 /* no libera el frame */

#endif
