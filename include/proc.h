#ifndef NETHEL_PROC_H
#define NETHEL_PROC_H
#include "file.h"
#include <stdint.h>

#define USER_MMAP_BASE  0x00007F0000000000ULL   /* mmap crece hacia arriba desde aca */
#define USER_MMAP_LIMIT 0x00007FFF00000000ULL

/* Estado de proceso que Linux/glibc esperan que el kernel recuerde */
#include "sig.h"
struct proc {
    uint64_t fs_base;        /* TLS (arch_prctl ARCH_SET_FS) */
    uint64_t clear_tid;      /* set_tid_address */
    uint64_t brk_start;      /* paso 3: brk */
    uint64_t brk_cur;
    uint64_t mmap_next;      /* paso 3: proxima direccion libre para mmap */
    struct file *fds[NFD];
    char cwd[128];
    int  exit_code;
    uint64_t sig_pending;
    uint64_t sig_blocked;
    int pgid, sid;                      /* 0 = el propio id */
    struct ksigaction sa[NSIG];
    int stopped;                        /* 0 = corriendo, si no: senal que lo detuvo */

    uint32_t ruid, euid, suid;
    uint32_t rgid, egid, sgid;
    uint32_t umask;
};

static inline void proc_clear(struct proc *p)
{
    uint8_t *b = (uint8_t *)p;
    for (unsigned i = 0; i < sizeof(*p); i++) b[i] = 0;
    p->umask = 022;
}

#endif
