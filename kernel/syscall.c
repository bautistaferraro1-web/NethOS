#include "syscall.h"
#include "console.h"
#include "sched.h"
#include "vmm.h"
#include "user.h"
#include "keyboard.h"
#include "proc.h"

#define SYS_READ  0
#define SYS_WRITE 1
#define SYS_YIELD 24
#define SYS_EXIT  60
#define SYS_SPAWN 500                      /* propias de Nethel */
#define SYS_WAIT  501
#define SYS_ARCH_PRCTL  158                /* Linux */
#define SYS_SET_TID     218
#define SYS_EXIT_GROUP  231

#define UBASE 0x0000000000400000ULL        /* region de usuario */
#define UEND  0x00007FFFFFFFF000ULL
#define MAXW  256

#define ERR(n) ((uint64_t)-(int64_t)(n))
#define ENOENT 2
#define EBADF  9
#define E2BIG  7
#define EFAULT 14
#define EINVAL 22

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
    while ((c = keyboard_getc()) < 0) {
        if (task_killed()) user_kill_self();
        __asm__ volatile("sti; hlt; cli" : : : "memory");   /* dormir hasta una IRQ */
    }

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

/* Copia un string del usuario (cap incluye el NUL). Devuelve el largo, -1 fault, -2 muy largo */
static int copy_str(char *dst, int cap, uint64_t src)
{
    for (int i = 0; i < cap; i++) {
        if (!user_range_ok(src + i, 1)) return -1;
        dst[i] = ((const char *)src)[i];
        if (!dst[i]) return i;
    }
    return -2;
}

/* spawn(nombre, argv): argv es un array de char* terminado en NULL (o 0) */
static uint64_t sys_spawn(uint64_t uname, uint64_t uargv)
{
    char name[16];
    struct uargs a;
    int off = 0;
    a.argc = 0;
    if (copy_name(name, uname)) return ERR(EFAULT);

    while (uargv) {
        if (!user_range_ok(uargv + 8ULL * a.argc, 8)) return ERR(EFAULT);
        uint64_t p = *(const uint64_t *)(uargv + 8ULL * a.argc);
        if (!p) break;
        if (a.argc >= USER_MAXARGS) return ERR(E2BIG);
        int n = copy_str(a.buf + off, USER_ARGBUF - off, p);
        if (n == -1) return ERR(EFAULT);
        if (n < 0)   return ERR(E2BIG);
        off += n + 1;
        a.argc++;
    }
    if (a.argc == 0) {                       /* sin argv: argv[0] = nombre */
        int n = 0;
        while (name[n]) { a.buf[n] = name[n]; n++; }
        a.buf[n] = 0;
        a.argc = 1;
    }
    int id = user_spawn_args(name, &a);
    return id < 0 ? ERR(ENOENT) : (uint64_t)id;
}

static uint64_t sys_wait(uint64_t id)
{
    while (task_alive((int)id)) {
        if (task_killed()) user_kill_self();
        yield();
        __asm__ volatile("sti; hlt; cli" : : : "memory");
    }
    return 0;
}

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr)
{
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= UEND) return ERR(EINVAL);            /* tiene que ser de usuario */
        task_set_fs(addr);
        return 0;
    case ARCH_GET_FS:
        if (!user_range_ok(addr, 8)) return ERR(EFAULT);
        *(uint64_t *)addr = task_proc()->fs_base;
        return 0;
    default:
        return ERR(EINVAL);
    }
}

/* Sin threads ni futex todavia: solo recordamos la direccion y devolvemos el tid */
static uint64_t sys_set_tid(uint64_t addr)
{
    task_proc()->clear_tid = addr;
    return (uint64_t)task_id();
}

void syscall_dispatch(struct regs *r)
{
    switch (r->rax) {
    case SYS_READ:  r->rax = sys_read(r->rdi, r->rsi, r->rdx); break;
    case SYS_WRITE: r->rax = sys_write(r->rdi, r->rsi, r->rdx); break;
    case SYS_YIELD: yield(); r->rax = 0; break;
    case SYS_SPAWN: r->rax = sys_spawn(r->rdi, r->rsi); break;
    case SYS_WAIT:  r->rax = sys_wait(r->rdi); break;
    case SYS_ARCH_PRCTL: r->rax = sys_arch_prctl(r->rdi, r->rsi); break;
    case SYS_SET_TID:    r->rax = sys_set_tid(r->rdi); break;
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        console_puts("[kernel] user exit("); console_dec(r->rdi); console_puts(")\n");
        user_cleanup();
        task_exit();
        for (;;) __asm__ volatile("hlt");
    default:
        r->rax = ERR(38);                  /* ENOSYS */
    }
}
