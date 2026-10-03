#include "syscall.h"
#include "console.h"
#include "sched.h"
#include "vmm.h"

#define SYS_WRITE 1
#define SYS_YIELD 24
#define SYS_EXIT  60

#define UBASE 0x0000008000000000ULL        /* region de usuario */
#define UEND  0x0000008000200000ULL
#define MAXW  256

#define ERR(n) ((uint64_t)-(int64_t)(n))
#define EBADF  9
#define EFAULT 14

/* El rango tiene que estar en la region de usuario y mapeado */
static int user_range_ok(uint64_t p, uint64_t len)
{
    if (p < UBASE || p >= UEND || len > UEND - p) return 0;
    for (uint64_t a = p & ~0xFFFULL; a < p + len; a += 4096) {
        uint64_t ph;
        if (vmm_translate(a, &ph)) return 0;
    }
    return 1;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len)
{
    if (fd != 1 && fd != 2) return ERR(EBADF);
    if (len > MAXW) len = MAXW;
    if (!user_range_ok(buf, len)) return ERR(EFAULT);
    const char *s = (const char *)buf;
    for (uint64_t i = 0; i < len; i++) console_putc(s[i]);
    return len;
}

void syscall_dispatch(struct regs *r)
{
    switch (r->rax) {
    case SYS_WRITE: r->rax = sys_write(r->rdi, r->rsi, r->rdx); break;
    case SYS_YIELD: yield(); r->rax = 0; break;
    case SYS_EXIT:
        console_puts("[kernel] user exit("); console_dec(r->rdi); console_puts(")\n");
        for (;;) yield();                  /* provisorio: falta liberar la tarea */
    default:
        r->rax = ERR(38);                  /* ENOSYS */
    }
}
