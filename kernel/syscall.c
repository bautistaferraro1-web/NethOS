#include "syscall.h"
#include "console.h"
#include "sched.h"
#include "vmm.h"
#include "user.h"
#include "keyboard.h"

#define SYS_READ  0
#define SYS_WRITE 1
#define SYS_YIELD 24
#define SYS_EXIT  60
#define SYS_SPAWN 500                      /* propias de Nethel */
#define SYS_WAIT  501

#define UBASE 0x0000008000000000ULL        /* region de usuario */
#define UEND  0x0000008000200000ULL
#define MAXW  256

#define ERR(n) ((uint64_t)-(int64_t)(n))
#define ENOENT 2
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

/* Bloquea hasta que haya al menos una tecla; devuelve lo que haya (max len) */
static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len)
{
    if (fd != 0) return ERR(EBADF);
    if (len > MAXW) len = MAXW;
    if (len == 0) return 0;
    if (!user_range_ok(buf, len)) return ERR(EFAULT);

    int c;
    while ((c = keyboard_getc()) < 0)
        __asm__ volatile("sti; hlt; cli" : : : "memory");   /* dormir hasta una IRQ */

    char *d = (char *)buf;
    uint64_t n = 0;
    d[n++] = (char)c;
    while (n < len && (c = keyboard_getc()) >= 0) d[n++] = (char)c;
    return n;
}

/* Copia un nombre del usuario (max 15 chars), validando cada byte */
static int copy_name(char *dst, uint64_t src)
{
    for (int i = 0; i < 15; i++) {
        if (!user_range_ok(src + i, 1)) return -1;
        dst[i] = ((const char *)src)[i];
        if (!dst[i]) return 0;
    }
    dst[15] = 0;
    return 0;
}

static uint64_t sys_spawn(uint64_t uname)
{
    char name[16];
    if (copy_name(name, uname)) return ERR(EFAULT);
    int id = user_spawn_name(name);
    return id < 0 ? ERR(ENOENT) : (uint64_t)id;
}

static uint64_t sys_wait(uint64_t id)
{
    while (task_alive((int)id)) {
        yield();
        __asm__ volatile("sti; hlt; cli" : : : "memory");
    }
    return 0;
}

void syscall_dispatch(struct regs *r)
{
    switch (r->rax) {
    case SYS_READ:  r->rax = sys_read(r->rdi, r->rsi, r->rdx); break;
    case SYS_WRITE: r->rax = sys_write(r->rdi, r->rsi, r->rdx); break;
    case SYS_YIELD: yield(); r->rax = 0; break;
    case SYS_SPAWN: r->rax = sys_spawn(r->rdi); break;
    case SYS_WAIT:  r->rax = sys_wait(r->rdi); break;
    case SYS_EXIT:
        console_puts("[kernel] user exit("); console_dec(r->rdi); console_puts(")\n");
        user_cleanup();
        task_exit();
        for (;;) __asm__ volatile("hlt");
    default:
        r->rax = ERR(38);                  /* ENOSYS */
    }
}
