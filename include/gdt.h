#ifndef NETHEL_GDT_H
#define NETHEL_GDT_H
#include <stdint.h>

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA (0x18 | 3)
#define SEL_UCODE (0x20 | 3)
#define SEL_TSS   0x28

void gdt_init(void);
/* Stack que usa la CPU al pasar de Ring 3 a Ring 0 */
void gdt_set_kernel_stack(uint64_t rsp0);

#endif
