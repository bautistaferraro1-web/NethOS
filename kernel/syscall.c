#include "syscall.h"
#include "console.h"
#include "sched.h"
#include "vmm.h"
#include "user.h"
#include "keyboard.h"
#include "proc.h"
#include "file.h"
#include "initramfs.h"
#include "pmm.h"
#include "mem.h"
#include "timer.h"

#define SYS_READ  0
#define SYS_WRITE 1
#define SYS_CLOSE 3
#define SYS_STAT 4
#define SYS_OPEN 2
#define SYS_LSEEK 8
#define SYS_OPENAT 257
#define SYS_GETCWD 79
#define SYS_CHDIR 80
#define SYS_GETDENTS64 217
#define SYS_GETPID 39
#define SYS_FORK 57
#define SYS_EXECVE 59
#define SYS_CLONE 56
#define SYS_WAIT4 61
#define SYS_GETPPID 110
#define SYS_IOCTL  16
#define SYS_WRITEV 20
#define SYS_YIELD 24
#define SYS_EXIT  60
#define SYS_SPAWN 500                      /* propias de Nethel */
#define SYS_WAIT  501
#define SYS_ARCH_PRCTL  158                /* Linux */
#define SYS_SET_TID     218
#define SYS_EXIT_GROUP  231

#define UBASE 0x0000000000400000ULL        /* region de usuario */
#define UEND  0x00007FFFFFFFF000ULL

#define ERR(n) ((uint64_t)-(int64_t)(n))
#define ENOENT 2
#define EBADF  9
#define E2BIG  7
#define ENOEXEC 8
#define EACCES 13
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

#define TCGETS 0x5401
#define KTERMIOS_SIZE 36
#define IOV_MAX 1024
struct nethel_iovec { uint64_t base, len; };

static struct file *cur_fd(uint64_t fd) { return fd_lookup(task_proc()->fds, fd); }

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len)
{
    struct file *f = cur_fd(fd);
    if (!f || !f->ops->write) return ERR(EBADF);
    if (!user_range_ok(buf, len)) return ERR(EFAULT);
    return (uint64_t)f->ops->write(f, (const void *)buf, len);
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len)
{
    struct file *f = cur_fd(fd);
    if (!f || !f->ops->read) return ERR(EBADF);
    if (len == 0) return 0;
    if (!user_range_ok(buf, len)) return ERR(EFAULT);
    return (uint64_t)f->ops->read(f, (void *)buf, len);
}

static uint64_t sys_writev(uint64_t fd, uint64_t uiov, uint64_t cnt)
{
    struct file *f = cur_fd(fd);
    if (!f || !f->ops->write) return ERR(EBADF);
    if (cnt > IOV_MAX) return ERR(EINVAL);
    if (cnt == 0) return 0;
    if (!user_range_ok(uiov, cnt * sizeof(struct nethel_iovec))) return ERR(EFAULT);

    const struct nethel_iovec *iov = (const struct nethel_iovec *)uiov;
    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {          /* validar todo antes de escribir */
        if (iov[i].len > (1ULL << 40) - total) return ERR(EINVAL);
        if (iov[i].len && !user_range_ok(iov[i].base, iov[i].len)) return ERR(EFAULT);
        total += iov[i].len;
    }
    for (uint64_t i = 0; i < cnt; i++) {
        if (!iov[i].len) continue;
        int64_t w = f->ops->write(f, (const void *)iov[i].base, iov[i].len);
        if (w < 0) return (uint64_t)w;
    }
    return total;
}

static uint64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg)
{
    struct file *f = cur_fd(fd);
    if (!f) return ERR(EBADF);
    if (!f->ops->ioctl) return ERR(ENOTTY);
    if (req == TCGETS && !user_range_ok(arg, KTERMIOS_SIZE)) return ERR(EFAULT);
    if (req == 0x802C542A && !user_range_ok(arg, 44)) return ERR(EFAULT);
    if (req == 0x540F && !user_range_ok(arg, 4)) return ERR(EFAULT);
    return (uint64_t)f->ops->ioctl(f, req, arg);
}

static uint64_t sys_fstat(uint64_t fd, uint64_t st)
{
    struct file *f = cur_fd(fd);
    if (!f) return ERR(EBADF);
    if (!user_range_ok(st, 144)) return ERR(EFAULT);
    for (int i = 0; i < 144; i++) ((uint8_t *)st)[i] = 0;
    return f->ops->stat ? (uint64_t)f->ops->stat(f, (void *)st) : ERR(EINVAL);
}

static uint64_t sys_close(uint64_t fd) { return (uint64_t)(int64_t)fd_close(task_proc()->fds, fd); }


/* Bloquea hasta que haya al menos una tecla; devuelve lo que haya (max len) */





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
    user_wait((int)id, 0, 1);               /* cosechar la entrada de la tabla */
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

#define SYS_CLOCK_GETTIME 228

/* Reloj monotonico a 100 Hz; sirve para cualquier clk_id por ahora */
static uint64_t sys_clock_gettime(uint64_t clk, uint64_t ts)
{
    if (clk > 11) return ERR(EINVAL);
    if (!user_range_ok(ts, 16)) return ERR(EFAULT);
    uint64_t t = timer_ticks();
    uint64_t *p = (uint64_t *)ts;
    p[0] = t / 100;
    p[1] = (t % 100) * 10000000ULL;
    return 0;
}

#define SYS_FSTAT 5
#define S_IFCHR_ 0020000

/* struct stat de x86_64 Linux: 144 bytes. fd 0/1/2 son el "terminal" (char device) */

#define EMFILE 24
#define ESPIPE 29
#define EROFS  30
#define ENAMETOOLONG 36
#define PATH_MAX_K 128

/* -1 = fault, -2 = demasiado largo */
static int copy_path(char *dst, uint64_t src)
{
    for (int i = 0; i < PATH_MAX_K; i++) {
        if (!user_range_ok(src + i, 1)) return -1;
        dst[i] = ((const char *)src)[i];
        if (!dst[i]) return 0;
    }
    return -2;
}

#define ENOTDIR 20
#define ERANGE  34

/* Ruta del usuario -> absoluta y normalizada. 0 ok, -1 fault, -2 larga */
static int user_path(char *out, uint64_t src)
{
    char raw[PATH_MAX_K], comb[2 * PATH_MAX_K];
    int r = copy_path(raw, src);
    if (r) return r;
    int n = 0;
    if (raw[0] != '/') {
        for (const char *c = task_proc()->cwd; *c; ) comb[n++] = *c++;
        comb[n++] = '/';
    }
    for (const char *c = raw; *c; ) comb[n++] = *c++;
    comb[n] = 0;

    int len = 0;
    const char *p = comb;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *q = p;
        while (*q && *q != '/') q++;
        int cl = (int)(q - p);
        if (cl == 1 && p[0] == '.') { }
        else if (cl == 2 && p[0] == '.' && p[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;
        } else {
            if (len + 1 + cl >= PATH_MAX_K) return -2;
            out[len++] = '/';
            for (int i = 0; i < cl; i++) out[len++] = p[i];
        }
        p = q;
    }
    if (len == 0) out[len++] = '/';
    out[len] = 0;
    return 0;
}

static uint64_t do_open(uint64_t upath, uint64_t flags)
{
    char p[PATH_MAX_K];
    int r = user_path(p, upath);
    if (r == -1) return ERR(EFAULT);
    if (r == -2) return ERR(ENAMETOOLONG);
    if (flags & 3) return ERR(EROFS);                /* solo lectura por ahora */
    struct file *f = initramfs_open(p);
    if (!f) return ERR(ENOENT);
    int fd = fd_install(task_proc()->fds, f);
    if (fd < 0) { file_put(f); return ERR(EMFILE); }
    return (uint64_t)fd;
}

static uint64_t sys_open(uint64_t path, uint64_t flags) { return do_open(path, flags); }
/* dirfd se ignora: todo es relativo a / */
static uint64_t sys_openat(uint64_t dirfd, uint64_t path, uint64_t flags) { (void)dirfd; return do_open(path, flags); }

static uint64_t sys_stat(uint64_t upath, uint64_t st)
{
    char p[PATH_MAX_K];
    int r = user_path(p, upath);
    if (r == -1) return ERR(EFAULT);
    if (r == -2) return ERR(ENAMETOOLONG);
    if (!user_range_ok(st, 144)) return ERR(EFAULT);
    struct file *f = initramfs_open(p);
    if (!f) return ERR(ENOENT);
    for (int i = 0; i < 144; i++) ((uint8_t *)st)[i] = 0;
    uint64_t rv = (uint64_t)f->ops->stat(f, (void *)st);
    file_put(f);
    return rv;
}

static uint64_t sys_lseek(uint64_t fd, uint64_t off, uint64_t whence)
{
    struct file *f = cur_fd(fd);
    if (!f) return ERR(EBADF);
    if (!f->ops->lseek) return ERR(ESPIPE);
    return (uint64_t)f->ops->lseek(f, (int64_t)off, (int)whence);
}

static uint64_t sys_getcwd(uint64_t buf, uint64_t size)
{
    const char *cwd = task_proc()->cwd;
    uint64_t len = 0;
    while (cwd[len]) len++;
    len++;
    if (size < len) return ERR(ERANGE);
    if (!user_range_ok(buf, len)) return ERR(EFAULT);
    for (uint64_t i = 0; i < len; i++) ((char *)buf)[i] = cwd[i];
    return len;
}

/* Sin directorios en el initramfs todavia: solo "/" es valido */
static uint64_t sys_chdir(uint64_t upath)
{
    char p[PATH_MAX_K];
    int r = user_path(p, upath);
    if (r == -1) return ERR(EFAULT);
    if (r == -2) return ERR(ENAMETOOLONG);
    int d = initramfs_isdir(p);
    if (d < 0) return ERR(ENOENT);
    if (!d) return ERR(ENOTDIR);
    char *cwd = task_proc()->cwd;
    int i = 0;
    while (p[i]) { cwd[i] = p[i]; i++; }
    cwd[i] = 0;
    return 0;
}

static uint64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t count)
{
    struct file *f = cur_fd(fd);
    if (!f) return ERR(EBADF);
    if (!f->ops->getdents) return ERR(ENOTDIR);
    if (!user_range_ok(buf, count)) return ERR(EFAULT);
    return (uint64_t)f->ops->getdents(f, (void *)buf, count);
}

static uint64_t sys_wait4(uint64_t pid, uint64_t ust, uint64_t opt, uint64_t ru)
{
    (void)ru;
    if (opt & ~(1ULL | 2ULL | 8ULL)) return ERR(EINVAL);   /* WNOHANG; WUNTRACED y WCONTINUED se ignoran (aun no hay stop) */
    if (ust && !user_range_ok(ust, 4)) return ERR(EFAULT);
    int st = 0;
    int r = user_wait((int)pid, &st, (int)(opt & 1));
    if (r > 0 && ust) *(int *)ust = st;
    return (uint64_t)(int64_t)r;
}

/* Entorno por defecto si el usuario no pasa envp */
static void env_default(struct uenv *e)
{
    static const char d[] = "PATH=/bin\0HOME=/";
    for (uint64_t i = 0; i < sizeof d; i++) e->buf[i] = d[i];
    e->envc = 2;
}

/* execve(path, argv, envp) */
static uint64_t sys_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp, struct regs *r)
{
    (void)uenvp;
    char path[PATH_MAX_K];
    struct uargs a;
    int off = 0;
    a.argc = 0;

    int pr = user_path(path, upath);
    if (pr == -1) return ERR(EFAULT);
    if (pr == -2) return ERR(ENAMETOOLONG);

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
    if (a.argc == 0) {
        int n = 0;
        while (path[n]) { a.buf[n] = path[n]; n++; }
        a.buf[n] = 0;
        a.argc = 1;
    }

    struct uenv ev;
    ev.envc = 0;
    int eoff = 0;
    while (uenvp) {
        if (!user_range_ok(uenvp + 8ULL * ev.envc, 8)) return ERR(EFAULT);
        uint64_t ep = *(const uint64_t *)(uenvp + 8ULL * ev.envc);
        if (!ep) break;
        if (ev.envc >= USER_MAXENV) return ERR(E2BIG);
        int en = copy_str(ev.buf + eoff, USER_ENVBUF - eoff, ep);
        if (en == -1) return ERR(EFAULT);
        if (en < 0)   return ERR(E2BIG);
        eoff += en + 1;
        ev.envc++;
    }
    if (ev.envc == 0) env_default(&ev);

    const uint8_t *img = 0;
    uint64_t size = 0;
    int lk = initramfs_lookup(path, &img, &size);
    if (lk == -1) return ERR(ENOENT);
    if (lk == -2) return ERR(EACCES);

    if (user_exec(img, size, &a, &ev, r)) return ERR(ENOEXEC);
    return 0;
}

static int zero_user(uint64_t p, uint64_t n)
{
    if (!p) return 0;
    if (!user_range_ok(p, n)) return -1;
    for (uint64_t i = 0; i < n; i++) ((uint8_t *)p)[i] = 0;
    return 0;
}

/* Todavia no hay entrega de senales: se acepta todo y no se guarda nada */
static uint64_t sys_sigaction(uint64_t sig, uint64_t act, uint64_t old)
{
    (void)sig; (void)act;
    if (zero_user(old, 32)) return ERR(EFAULT);
    return 0;
}

static uint64_t sys_sigprocmask(uint64_t how, uint64_t set, uint64_t old)
{
    (void)how; (void)set;
    if (zero_user(old, 8)) return ERR(EFAULT);
    return 0;
}

static uint64_t sys_getres(uint64_t a, uint64_t b, uint64_t c)
{
    if (zero_user(a, 4) || zero_user(b, 4) || zero_user(c, 4)) return ERR(EFAULT);
    return 0;
}

static uint64_t sys_gettimeofday(uint64_t tv, uint64_t tz)
{
    (void)tz;
    if (!tv) return 0;
    if (!user_range_ok(tv, 16)) return ERR(EFAULT);
    uint64_t t = timer_ticks();
    ((uint64_t *)tv)[0] = t / 100;
    ((uint64_t *)tv)[1] = (t % 100) * 10000ULL;
    return 0;
}

static void put_field(char *dst, const char *s)
{
    int i = 0;
    while (s[i] && i < 64) { dst[i] = s[i]; i++; }
}

static uint64_t sys_uname(uint64_t u)
{
    if (zero_user(u, 390) || !u) return ERR(EFAULT);
    char *b = (char *)u;
    put_field(b + 0,   "Linux");
    put_field(b + 65,  "nethel");
    put_field(b + 130, "6.1.0");
    put_field(b + 195, "#1 Nethel");
    put_field(b + 260, "x86_64");
    return 0;
}

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030

static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    struct file *f = cur_fd(fd);
    if (!f) return ERR(EBADF);
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        struct file **fds = task_proc()->fds;
        for (uint64_t i = arg; i < NFD; i++) {
            if (!fds[i]) { fds[i] = file_get(f); return i; }
        }
        return ERR(EMFILE);
    }
    case F_GETFD: case F_SETFD: case F_GETFL: case F_SETFL:
        return 0;
    default:
        return ERR(EINVAL);
    }
}

static uint64_t sys_dup(uint64_t fd)
{
    struct file *f = cur_fd(fd);
    if (!f) return ERR(EBADF);
    int n = fd_install(task_proc()->fds, file_get(f));
    if (n < 0) { file_put(f); return ERR(EMFILE); }
    return (uint64_t)n;
}

static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd)
{
    struct file *f = cur_fd(oldfd);
    if (!f || newfd >= NFD) return ERR(EBADF);
    if (oldfd == newfd) return newfd;
    struct file **fds = task_proc()->fds;
    if (fds[newfd]) fd_close(fds, newfd);
    fds[newfd] = file_get(f);
    return newfd;
}

/* No hay symlinks en el initramfs */
static uint64_t sys_readlink(uint64_t upath, uint64_t buf, uint64_t n)
{
    (void)upath; (void)buf; (void)n;
    return ERR(EINVAL);
}

#define AT_EMPTY_PATH_ 0x1000
/* dirfd se ignora salvo con AT_EMPTY_PATH (= fstat) */
static uint64_t sys_newfstatat(uint64_t dirfd, uint64_t upath, uint64_t st, uint64_t flags)
{
    if (!user_range_ok(st, 144)) return ERR(EFAULT);
    if (user_range_ok(upath, 1) && ((const char *)upath)[0] == 0) {
        if (flags & AT_EMPTY_PATH_) return sys_fstat(dirfd, st);
        return ERR(ENOENT);
    }
    return sys_stat(upath, st);
}

static uint64_t sys_access(uint64_t upath, uint64_t mode)
{
    (void)mode;
    char p[PATH_MAX_K];
    int r = user_path(p, upath);
    if (r == -1) return ERR(EFAULT);
    if (r == -2) return ERR(ENAMETOOLONG);
    struct file *f = initramfs_open(p);
    if (!f) return ERR(ENOENT);
    file_put(f);
    return 0;
}

static uint64_t sys_pipe2(uint64_t ufds, uint64_t flags)
{
    (void)flags;
    if (!user_range_ok(ufds, 8)) return ERR(EFAULT);
    struct file *rd, *wr;
    if (pipe_create(&rd, &wr)) return ERR(ENOMEM);
    struct file **fds = task_proc()->fds;
    int a = fd_install(fds, rd);
    if (a < 0) { file_put(rd); file_put(wr); return ERR(EMFILE); }
    int b = fd_install(fds, wr);
    if (b < 0) { fd_close(fds, (uint64_t)a); file_put(wr); return ERR(EMFILE); }
    ((int *)ufds)[0] = a;
    ((int *)ufds)[1] = b;
    return 0;
}

void syscall_dispatch(struct regs *r)
{
    switch (r->rax) {
    case SYS_READ:  r->rax = sys_read(r->rdi, r->rsi, r->rdx); break;
    case SYS_WRITE: r->rax = sys_write(r->rdi, r->rsi, r->rdx); break;
    case SYS_CLOSE: r->rax = sys_close(r->rdi); break;
    case SYS_OPEN:   r->rax = sys_open(r->rdi, r->rsi); break;
    case SYS_OPENAT: r->rax = sys_openat(r->rdi, r->rsi, r->rdx); break;
    case SYS_STAT:   r->rax = sys_stat(r->rdi, r->rsi); break;
    case 6:   r->rax = sys_stat(r->rdi, r->rsi); break;   /* lstat: sin symlinks es igual a stat */
    case 22:  r->rax = sys_pipe2(r->rdi, 0); break;
    case 293: r->rax = sys_pipe2(r->rdi, r->rsi); break;
    case 157: r->rax = 0; break;                          /* prctl: provisorio, acepta todo */
    case SYS_LSEEK:  r->rax = sys_lseek(r->rdi, r->rsi, r->rdx); break;
    case SYS_GETCWD: r->rax = sys_getcwd(r->rdi, r->rsi); break;
    case SYS_CHDIR:  r->rax = sys_chdir(r->rdi); break;
    case SYS_GETDENTS64: r->rax = sys_getdents64(r->rdi, r->rsi, r->rdx); break;
    case 502: r->rax = (uint64_t)user_clone_test(); break;   /* temporal */
    case SYS_FORK: {
        int id = user_fork(r);
        r->rax = id < 0 ? ERR(ENOMEM) : (uint64_t)id;
        break;
    }
    case SYS_CLONE: {                         /* solo el fork clasico (stack 0), como lo usa glibc */
        if (r->rsi != 0) { r->rax = ERR(EINVAL); break; }
        int id = user_fork(r);
        r->rax = id < 0 ? ERR(ENOMEM) : (uint64_t)id;
        break;
    }
    case SYS_EXECVE: r->rax = sys_execve(r->rdi, r->rsi, r->rdx, r); break;
    case 13:  r->rax = sys_sigaction(r->rdi, r->rsi, r->rdx); break;
    case 14:  r->rax = sys_sigprocmask(r->rdi, r->rsi, r->rdx); break;
    case 118: r->rax = sys_getres(r->rdi, r->rsi, r->rdx); break;
    case 120: r->rax = sys_getres(r->rdi, r->rsi, r->rdx); break;
    case 96:  r->rax = sys_gettimeofday(r->rdi, r->rsi); break;
    case 63:  r->rax = sys_uname(r->rdi); break;
    case 111: r->rax = (uint64_t)task_id(); break;
    case 72:  r->rax = sys_fcntl(r->rdi, r->rsi, r->rdx); break;
    case 32:  r->rax = sys_dup(r->rdi); break;
    case 33:  r->rax = sys_dup2(r->rdi, r->rsi); break;
    case 89:  r->rax = sys_readlink(r->rdi, r->rsi, r->rdx); break;
    case 262: r->rax = sys_newfstatat(r->rdi, r->rsi, r->rdx, r->r10); break;
    case 102: case 104: case 107: case 108: r->rax = 0; break;
    case 21:  r->rax = sys_access(r->rdi, r->rsi); break;
    case 269: r->rax = sys_access(r->rsi, r->rdx); break;
    case 109: r->rax = 0; break;
    case 121: case 112: r->rax = (uint64_t)task_id(); break;
    case SYS_GETPID:  r->rax = (uint64_t)task_id(); break;
    case SYS_GETPPID: r->rax = (uint64_t)user_ppid(); break;
    case SYS_WAIT4:   r->rax = sys_wait4(r->rdi, r->rsi, r->rdx, r->r10); break;
    case SYS_IOCTL:  r->rax = sys_ioctl(r->rdi, r->rsi, r->rdx); break;
    case SYS_WRITEV: r->rax = sys_writev(r->rdi, r->rsi, r->rdx); break;
    case SYS_YIELD: yield(); r->rax = 0; break;
    case SYS_SPAWN: r->rax = sys_spawn(r->rdi, r->rsi); break;
    case SYS_WAIT:  r->rax = sys_wait(r->rdi); break;
    case SYS_ARCH_PRCTL: r->rax = sys_arch_prctl(r->rdi, r->rsi); break;
    case SYS_SET_TID:    r->rax = sys_set_tid(r->rdi); break;
    case SYS_BRK:        r->rax = sys_brk(r->rdi); break;
    case SYS_MMAP:       r->rax = sys_mmap(r->rdi, r->rsi, r->rdx, r->r10, r->r8); break;
    case SYS_MPROTECT:
        r->rax = sys_mprotect(r->rdi, r->rsi, r->rdx); break;
    case SYS_MUNMAP:     r->rax = sys_munmap(r->rdi, r->rsi); break;
    case SYS_FSTAT:      r->rax = sys_fstat(r->rdi, r->rsi); break;
    case SYS_CLOCK_GETTIME: r->rax = sys_clock_gettime(r->rdi, r->rsi); break;
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        console_puts("[kernel] user exit("); console_dec(r->rdi); console_puts(")\n");
        task_proc()->exit_code = (int)((r->rdi & 0xFF) << 8);
        user_cleanup();
        task_exit();
        for (;;) __asm__ volatile("hlt");
    default:
        switch (r->rax) {
        case 40: case 273: case 334: case 302: case 318: case 267: case 262: break;   /* esperadas */
        default: console_puts("[kernel] syscall sin implementar: "); console_dec(r->rax); console_putc('\n');
        }
        r->rax = ERR(38);                  /* ENOSYS */
    }
}
