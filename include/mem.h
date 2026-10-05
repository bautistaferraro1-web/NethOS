#ifndef NETHEL_MEM_H
#define NETHEL_MEM_H
#include <stdint.h>

/* Direct map: toda la RAM fisica (hasta 4 GiB) visible en la mitad alta.
   Kernel: la imagen esta enlazada en KERNEL_VMA + direccion fisica. */
#if defined(VMM_HOST) || defined(HOST_TEST)
#define PHYS_MAP_BASE 0ULL                      /* tests en el host: identidad */
#define KERNEL_VMA    0ULL
#else
#define PHYS_MAP_BASE 0xFFFF800000000000ULL
#define KERNEL_VMA    0xFFFFFFFF80000000ULL
#endif

#define P2V(p) ((uint64_t)(p) + PHYS_MAP_BASE)
#define V2P(v) ((uint64_t)(v) - PHYS_MAP_BASE)
#define K2P(v) ((uint64_t)(v) - KERNEL_VMA)     /* simbolo del kernel -> fisica */

#endif
