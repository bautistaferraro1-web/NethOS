#include "syscall.h"
#include "console.h"
#include "sched.h"
#include "vmm.h"
#include "user.h"
#include "keyboard.h"
#include "proc.h"
#include "pmm.h"
#include "mem.h"

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
#define ENOMEM 12
#define ENODEV 19

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

#define SYS_MMAP     9
#define SYS_MPROTECT 10
#define SYS_MUNMAP   11
#define SYS_BRK      12

#define MAP_FIXED 0x10
#define MAP_ANON  0x20
#define BRK_MAX   (128ULL << 20)
#define MEM_MAXLEN (1ULL << 32)
#define PG_UP(x)  (((x) + 0xFFFULL) & ~0xFFFULL)

/* PROT_NONE = pagina presente sin bit U (el usuario falla, el kernel no) */
static uint64_t prot_flags(uint64_t prot)
{
    if (!(prot & 3)) return 0;
    return VMM_USER | ((prot & 2) ? VMM_WRITE : 0);
}

/* Desmapea y libera las paginas que existan en [va, va+len). Corre con IF=0. */
static void free_range(uint64_t va, uint64_t len)
{
    uint64_t sp = task_get_space();
    for (uint64_t a = va; a < va + len; a += 4096) {
        uint64_t ph;
        if (vmm_translate(a, &ph)) continue;
        if (vmm_unmap_in(sp, a) == 0) pmm_free_page(ph & ~0xFFFULL);
    }
}

/* Mapea paginas nuevas en cero. Si falta memoria deja todo como estaba. */
static int alloc_range(uint64_t va, uint64_t len, uint64_t flags)
{
    uint64_t sp = task_get_space();
    for (uint64_t a = va; a < va + len; a += 4096) {
        uint64_t f = pmm_alloc_page();
        if (!f) { free_range(va, a - va); return -1; }
        volatile uint64_t *z = (volatile uint64_t *)P2V(f);
        for (int i = 0; i < 512; i++) z[i] = 0;
        if (vmm_map_in(sp, a, f, flags)) { pmm_free_page(f); free_range(va, a - va); return -1; }
    }
    return 0;
}

static uint64_t sys_brk(uint64_t want)
{
    struct proc *p = task_proc();
    uint64_t cur = p->brk_cur;
    if (!p->brk_start || want < p->brk_start || want > p->brk_start + BRK_MAX) return cur;

    uint64_t oldp = PG_UP(cur), newp = PG_UP(want);
    if (newp > oldp) {
        if (alloc_range(oldp, newp - oldp, VMM_USER | VMM_WRITE)) return cur;
    } else if (newp < oldp) {
        free_range(newp, oldp - newp);
    }
    p->brk_cur = want;
    return want;
}

static uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd)
{
    (void)fd;
    struct proc *p = task_proc();
    if (!len) return ERR(EINVAL);
    if (!(flags & MAP_ANON)) return ERR(ENODEV);             /* sin archivos todavia */
    if (len > MEM_MAXLEN) return ERR(ENOMEM);
    len = PG_UP(len);

    uint64_t va;
    if (flags & MAP_FIXED) {
        if ((addr & 0xFFF) || addr < UBASE || addr >= UEND || len > UEND - addr) return ERR(EINVAL);
        va = addr;
        free_range(va, len);                                 /* reemplaza lo que hubiera */
    } else {
        if (len > USER_MMAP_LIMIT - p->mmap_next) return ERR(ENOMEM);
        va = p->mmap_next;
    }
    if (alloc_range(va, len, prot_flags(prot))) return ERR(ENOMEM);
    if (!(flags & MAP_FIXED)) p->mmap_next = va + len;
    return va;
}

static uint64_t sys_munmap(uint64_t addr, uint64_t len)
{
    if ((addr & 0xFFF) || !len || len > MEM_MAXLEN) return ERR(EINVAL);
    len = PG_UP(len);
    if (addr < UBASE || addr >= UEND || len > UEND - addr) return ERR(EINVAL);
    free_range(addr, len);
    return 0;
}

static uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
{
    if ((addr & 0xFFF) || len > MEM_MAXLEN) return ERR(EINVAL);
    len = PG_UP(len);
    if (!len) return 0;
    if (addr < UBASE || addr >= UEND || len > UEND - addr) return ERR(ENOMEM);

    for (uint64_t a = addr; a < addr + len; a += 4096) {     /* todo mapeado, o no toca nada */
        uint64_t ph;
        if (vmm_translate(a, &ph)) return ERR(ENOMEM);
    }
    uint64_t sp = task_get_space(), fl = prot_flags(prot);
    for (uint64_t a = addr; a < addr + len; a += 4096) {
        uint64_t ph;
        vmm_translate(a, &ph);
        vmm_unmap_in(sp, a);
        vmm_map_in(sp, a, ph & ~0xFFFULL, fl);
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
    case SYS_BRK:        r->rax = sys_brk(r->rdi); break;
    case SYS_MMAP:       r->rax = sys_mmap(r->rdi, r->rsi, r->rdx, r->r10, r->r8); break;
    case SYS_MPROTECT:   r->rax = sys_mprotect(r->rdi, r->rsi, r->rdx); break;
    case SYS_MUNMAP:     r->rax = sys_munmap(r->rdi, r->rsi); break;
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
