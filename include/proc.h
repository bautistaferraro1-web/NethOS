#ifndef NETHEL_PROC_H
#define NETHEL_PROC_H
#include <stdint.h>

/* Estado de proceso que Linux/glibc esperan que el kernel recuerde */
struct proc {
    uint64_t fs_base;        /* TLS (arch_prctl ARCH_SET_FS) */
    uint64_t clear_tid;      /* set_tid_address */
    uint64_t brk_start;      /* paso 3: brk */
    uint64_t brk_cur;
    uint64_t mmap_next;      /* paso 3: proxima direccion libre para mmap */
};

static inline void proc_clear(struct proc *p)
{
    uint64_t *w = (uint64_t *)p;
    for (unsigned i = 0; i < sizeof(*p) / 8; i++) w[i] = 0;
}

#endif
