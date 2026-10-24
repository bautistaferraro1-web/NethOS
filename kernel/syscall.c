#include "sig.h"
#ifndef EINTR
#define EINTR 4
#endif
#include "syscall.h"
#include "console.h"
#include "sched.h"
#include "vmm.h"
#include "user.h"
#include "keyboard.h"
#include "proc.h"
#include "file.h"
#include "initramfs.h"
#include "tmpfs.h"
#include "fat32.h"
#include "pmm.h"
#include "mem.h"
#include "timer.h"
#include "power.h"
#include "rtc.h"

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
#define SYS_REBOOT 169
#define SYS_SYNC 162

#define UBASE 0x0000000000400000ULL        /* region de usuario */
#define UEND  0x00007FFFFFFFF000ULL

#define ERR(n) ((uint64_t)-(int64_t)(n))
#define ENOENT 2
#define EPERM  1
#define EBADF  9
#define E2BIG  7
#define ENOEXEC 8
#define EACCES 13
#define EFAULT 14
#define EINVAL 22
#define ENOMEM 12
#define ENODEV 19
#define EIO 5

/* Linux reboot(2): validación del ABI antes de tocar el hardware. */
static uint64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd)
{
    if (magic1 != 0xFEE1DEADULL)
        return ERR(EINVAL);

    if (magic2 != 0x28121969ULL &&
        magic2 != 0x05121996ULL &&
        magic2 != 0x16041998ULL &&
        magic2 != 0x20112000ULL)
        return ERR(EINVAL);

    switch (cmd) {
    case 0x01234567ULL: /* LINUX_REBOOT_CMD_RESTART */
    case 0xA1B2C3D4ULL: /* LINUX_REBOOT_CMD_RESTART2 */
        power_reboot();

    case 0x4321FEDCULL: /* LINUX_REBOOT_CMD_POWER_OFF */
    case 0xCDEF0123ULL: /* LINUX_REBOOT_CMD_HALT: detener la VM */
        power_poweroff();

    default:
        return ERR(EINVAL);
    }
}

/* El rango tiene que estar en la region de usuario y mapeado */
int user_range_ok(uint64_t p, uint64_t len)
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

static struct file *cur_fd(uint64_t fd)
{
    return fd_lookup(task_proc()->fds, fd);
}

/* requested: R=4, W=2, X=1.
   real_ids=1 usa ruid/rgid para access(); en otro caso usa euid/egid. */
static int file_access_allowed(struct file *f, unsigned requested, int real_ids)
{
    uint8_t st[144] = {0};

    if (!f || !f->ops || !f->ops->stat ||
        f->ops->stat(f, st) < 0)
        return 0;

    uint32_t mode = *(uint32_t *)(st + 24);
    uint32_t owner = *(uint32_t *)(st + 28);
    uint32_t group = *(uint32_t *)(st + 32);

    struct proc *p = task_proc();
    uint32_t uid = real_ids ? p->ruid : p->euid;
    uint32_t gid = real_ids ? p->rgid : p->egid;

    /* F_OK: basta con que el objeto exista. */
    if (!requested)
        return 1;

    /* Root puede leer y escribir; para ejecutar un archivo regular,
       debe existir al menos un bit de ejecución. */
    if (uid == 0) {
        if ((requested & 1u) &&
            (mode & 0170000u) == 0100000u &&
            !(mode & 0111u))
            return 0;
        return 1;
    }

    unsigned bits;
    if (uid == owner)
        bits = (mode >> 6) & 7u;
    else if (gid == group)
        bits = (mode >> 3) & 7u;
    else
        bits = mode & 7u;

    return (bits & requested) == requested;
}

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
    if ((req == TCGETS || (req >= 0x5402 && req <= 0x5404)) && !user_range_ok(arg, KTERMIOS_SIZE)) return ERR(EFAULT);
    if (req == 0x5413 && !user_range_ok(arg, 8)) return ERR(EFAULT);
    if (req == 0x802C542A && !user_range_ok(arg, 44)) return ERR(EFAULT);
    if ((req == 0x540F || req == 0x5410) && !user_range_ok(arg, 4)) return ERR(EFAULT);
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
        if (task_sigpending()) return ERR(EINTR);
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

#define SYS_TIME 201

static uint64_t sys_time(uint64_t tloc)
{
    if (tloc && !user_range_ok(tloc, sizeof(int64_t)))
        return ERR(EFAULT);

    int64_t now;
    if (rtc_get_unix_seconds(&now) != 0)
        return ERR(EIO);

    if (tloc)
        *(int64_t *)tloc = now;

    return (uint64_t)now;
}

#define SYS_CLOCK_GETTIME 228

/* Reloj monotonico a 100 Hz; sirve para cualquier clk_id por ahora */
static uint64_t sys_clock_gettime(uint64_t clk, uint64_t ts)
{
    if (clk != 0 && clk != 1)
        return ERR(EINVAL);

    if (!user_range_ok(ts, 16))
        return ERR(EFAULT);

    uint64_t sec, nsec;

    if (clk == 0) {
        int64_t now;
        if (rtc_get_unix_seconds(&now) != 0)
            return ERR(EIO);
        sec = (uint64_t)now;
        nsec = 0;
    } else {
        uint64_t t = timer_ticks();
        sec = t / 100;
        nsec = (t % 100) * 10000000ULL;
    }

    uint64_t *p = (uint64_t *)ts;
    p[0] = sec;
    p[1] = nsec;
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

/* tmpfs primero, despues initramfs */
static struct file *open_any(const char *p)
{
    if (fat32_handles_path(p))
        return fat32_open(p);

    int e = 0;
    struct file *f = tmpfs_open(p, 0, &e);
    if (f) return f;
    return initramfs_open(p);
}

/* Comprueba tipo y permisos de un directorio. Devuelve errno positivo o cero. */
static int check_directory_access(const char *path, unsigned requested)
{
    struct file *f = open_any(path);
    if (!f) return ENOENT;

    uint8_t st[144] = {0};
    if (!f->ops || !f->ops->stat || f->ops->stat(f, st) < 0) {
        file_put(f);
        return EIO;
    }

    uint32_t mode = *(uint32_t *)(st + 24);
    if ((mode & 0170000u) != 0040000u) {
        file_put(f);
        return ENOTDIR;
    }

    int allowed = file_access_allowed(f, requested, 0);
    file_put(f);
    return allowed ? 0 : EACCES;
}

/* Busca permiso X en cada directorio padre de una ruta absoluta. */
static int check_path_parents(const char *path)
{
    if (!path || path[0] != '/')
        return EINVAL;

    int rc = check_directory_access("/", 1);
    if (rc) return rc;
    if (path[1] == 0) return 0;

    char prefix[PATH_MAX_K] = "/";
    int plen = 1;
    const char *p = path + 1;

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;

        /* El último componente es el objeto solicitado, no un directorio padre. */
        if (!*p) break;

        int n = (int)(p - start);
        int sep = plen > 1 ? 1 : 0;
        if (plen + sep + n >= PATH_MAX_K)
            return 36; /* ENAMETOOLONG */

        if (sep) prefix[plen++] = '/';
        for (int i = 0; i < n; i++)
            prefix[plen++] = start[i];
        prefix[plen] = 0;

        rc = check_directory_access(prefix, 1);
        if (rc) return rc;

        while (*p == '/') p++;
    }

    return 0;
}

/* Crear o borrar un nombre requiere escritura y búsqueda en el padre. */
static int check_parent_access(const char *path)
{
    int last = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last = i;

    if (last < 0) return EINVAL;

    char parent[PATH_MAX_K];
    if (last == 0) {
        parent[0] = '/';
        parent[1] = 0;
    } else {
        if (last >= PATH_MAX_K) return 36;
        for (int i = 0; i < last; i++) parent[i] = path[i];
        parent[last] = 0;
    }

    int rc = check_path_parents(parent);
    if (rc) return rc;

    return check_directory_access(parent, 3); /* W | X */
}

static uint64_t do_open(uint64_t upath, uint64_t flags, uint64_t mode)
{
    char p[PATH_MAX_K];
    int r = user_path(p, upath);
    if (r == -1) return ERR(EFAULT);
    if (r == -2) return ERR(ENAMETOOLONG);

    int pe = check_path_parents(p);
    if (pe) return ERR(pe);

    int acc = (int)(flags & 3);
    if (acc == 3) return ERR(EINVAL);

    /* O_CREAT | O_EXCL debe fallar si el nombre ya existe. */
    struct file *probe = open_any(p);
    int exists = probe != 0;
    if (probe) {
        if ((flags & 0100) && (flags & 0200)) {
            file_put(probe);
            return ERR(17); /* EEXIST */
        }

        unsigned requested = acc == 0 ? 4u : acc == 1 ? 2u : 6u;
        if (flags & 01000) requested |= 2u; /* O_TRUNC requiere escritura */

        int allowed = file_access_allowed(probe, requested, 0);
        file_put(probe);

        if (!allowed) return ERR(EACCES);
    }

    if (!exists && (flags & 0100)) {
        int pe = check_parent_access(p);
        if (pe) return ERR(pe);
    }

    int wr = (flags & 3) || (flags & 0100) || (flags & 01000);
    struct file *f;
    if (!wr) {
        f = open_any(p);
    } else if (fat32_handles_path(p)) {
        int err = 0;
        f = fat32_open_flags(p, (int)flags, &err);
        if (!f) return ERR(err ? err : EROFS);
    } else {
        if (!tmpfs_exists(p)) {
            struct file *ro = initramfs_open(p);
            if (ro) { file_put(ro); return ERR(EROFS); }
        }
        int err = 0;
        struct proc *pr = task_proc();
        f = tmpfs_open_as(p, (int)flags, (uint32_t)mode,
                          pr->euid, pr->egid, pr->umask, &err);
        if (!f) return ERR(err);
    }
    if (!f) return ERR(ENOENT);
    int fd = fd_install(task_proc()->fds, f);
    if (fd < 0) { file_put(f); return ERR(EMFILE); }
    return (uint64_t)fd;
}

static uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode)
{
    return do_open(path, flags, mode);
}
/* dirfd se ignora: todo es relativo a / */
static uint64_t sys_openat(uint64_t dirfd, uint64_t path,
                           uint64_t flags, uint64_t mode)
{
    (void)dirfd;
    return do_open(path, flags, mode);
}

static uint64_t sys_stat(uint64_t upath, uint64_t st)
{
    char p[PATH_MAX_K];
    int r = user_path(p, upath);
    if (r == -1) return ERR(EFAULT);
    if (r == -2) return ERR(ENAMETOOLONG);
    int pe = check_path_parents(p);
    if (pe) return ERR(pe);
    if (!user_range_ok(st, 144)) return ERR(EFAULT);
    struct file *f = open_any(p);
    if (!f) return ERR(ENOENT);
    for (int i = 0; i < 144; i++) ((uint8_t *)st)[i] = 0;
    uint64_t rv = (uint64_t)f->ops->stat(f, (void *)st);
    file_put(f);
    return rv;
}

static uint64_t sys_chmod(uint64_t upath, uint64_t mode)
{
    char path[PATH_MAX_K];
    int rc = user_path(path, upath);
    if (rc == -1) return ERR(EFAULT);
    if (rc == -2) return ERR(ENAMETOOLONG);
    int pe = check_path_parents(path);
    if (pe) return ERR(pe);

    struct proc *p = task_proc();

    if (tmpfs_exists(path)) {
        rc = tmpfs_chmod(path, (uint32_t)mode, p->euid);
        return rc < 0 ? ERR(-rc) : 0;
    }

    struct file *f = open_any(path);
    if (!f) return ERR(ENOENT);
    file_put(f);
    return ERR(30);                    /* EROFS: filesystem de solo lectura */
}

static uint64_t sys_chown(uint64_t upath, uint64_t uid, uint64_t gid)
{
    char path[PATH_MAX_K];
    int rc = user_path(path, upath);
    if (rc == -1) return ERR(EFAULT);
    if (rc == -2) return ERR(ENAMETOOLONG);
    int pe = check_path_parents(path);
    if (pe) return ERR(pe);

    struct proc *p = task_proc();

    if (tmpfs_exists(path)) {
        rc = tmpfs_chown(path, (uint32_t)uid, (uint32_t)gid, p->euid);
        return rc < 0 ? ERR(-rc) : 0;
    }

    struct file *f = open_any(path);
    if (!f) return ERR(ENOENT);
    file_put(f);
    return ERR(30);                    /* EROFS */
}

static uint64_t sys_fchmod(uint64_t fd, uint64_t mode)
{
    struct file *f = cur_fd(fd);
    if (!f) return ERR(EBADF);

    int rc = tmpfs_fchmod(f, (uint32_t)mode, task_proc()->euid);
    return rc < 0 ? ERR(-rc) : 0;
}

static uint64_t sys_fchown(uint64_t fd, uint64_t uid, uint64_t gid)
{
    struct file *f = cur_fd(fd);
    if (!f) return ERR(EBADF);

    int rc = tmpfs_fchown(f, (uint32_t)uid, (uint32_t)gid,
                          task_proc()->euid);
    return rc < 0 ? ERR(-rc) : 0;
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

/* Validar tipo y permiso de búsqueda sobre el directorio destino. */
static uint64_t sys_chdir(uint64_t upath)
{
    char p[PATH_MAX_K];
    int r = user_path(p, upath);
    if (r == -1) return ERR(EFAULT);
    if (r == -2) return ERR(ENAMETOOLONG);
    int pe = check_path_parents(p);
    if (pe) return ERR(pe);

    struct file *f = open_any(p);
    if (!f) return ERR(ENOENT);

    uint8_t st[144] = {0};
    if (!f->ops || !f->ops->stat || f->ops->stat(f, st) < 0) {
        file_put(f);
        return ERR(ENOTDIR);
    }

    uint32_t mode = *(uint32_t *)(st + 24);
    if ((mode & 0170000u) != 0040000u) {
        file_put(f);
        return ERR(ENOTDIR);
    }

    int allowed = file_access_allowed(f, 1, 0); /* X: búsqueda */
    file_put(f);
    if (!allowed) return ERR(EACCES);

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
    if (!file_access_allowed(f, 4, 0)) return ERR(EACCES); /* R: listar */
    if (!user_range_ok(buf, count)) return ERR(EFAULT);
    return (uint64_t)f->ops->getdents(f, (void *)buf, count);
}

/* poll minimo: todo fd valido se reporta listo para leer/escribir (read bloquea si hace falta) */
struct kpollfd { int32_t fd; int16_t events, revents; };

static uint64_t sys_poll(uint64_t ufds, uint64_t nfds, uint64_t timeout)
{
    (void)timeout;
    if (nfds > 64) return ERR(EINVAL);
    if (nfds && !user_range_ok(ufds, nfds * sizeof(struct kpollfd))) return ERR(EFAULT);
    struct kpollfd *p = (struct kpollfd *)ufds;
    uint64_t ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        p[i].revents = 0;
        if (p[i].fd < 0) continue;
        if (!fd_lookup(task_proc()->fds, (uint64_t)p[i].fd)) { p[i].revents = 0x20; ready++; continue; }
        p[i].revents = p[i].events & (0x001 | 0x004);
        if (p[i].revents) ready++;
    }
    return ready;
}


/*
 * pselect6 reducido para Nethra.
 * Admite fd 0 para la consola y descriptores validos hasta nfds <= 64.
 * Las mascaras de señales no nulas aun no estan implementadas.
 */
static uint64_t sys_pselect6(uint64_t nfds, uint64_t ur, uint64_t uw,
                             uint64_t ux, uint64_t ut, uint64_t us)
{
    if (nfds > 64) return ERR(EINVAL);

    uint64_t bytes = nfds ? 8 : 0;
    if (bytes && ur && !user_range_ok(ur, bytes)) return ERR(EFAULT);
    if (bytes && uw && !user_range_ok(uw, bytes)) return ERR(EFAULT);
    if (bytes && ux && !user_range_ok(ux, bytes)) return ERR(EFAULT);

    uint64_t rin = (bytes && ur) ? *(const uint64_t *)ur : 0;
    uint64_t win = (bytes && uw) ? *(const uint64_t *)uw : 0;
    uint64_t xin = (bytes && ux) ? *(const uint64_t *)ux : 0;

    if (nfds < 64) {
        uint64_t mask = nfds ? ((1ULL << nfds) - 1ULL) : 0;
        rin &= mask;
        win &= mask;
        xin &= mask;
    }

    /* Un descriptor solicitado pero inválido debe devolver EBADF. */
    for (uint64_t fd = 0; fd < nfds; fd++) {
        uint64_t bit = 1ULL << fd;
        if (((rin | win | xin) & bit) &&
            !fd_lookup(task_proc()->fds, fd))
            return ERR(EBADF);
    }

    uint64_t deadline = 0;
    int has_timeout = ut != 0;

    if (has_timeout) {
        if (!user_range_ok(ut, 16)) return ERR(EFAULT);

        const int64_t *ts = (const int64_t *)ut;
        int64_t sec = ts[0];
        int64_t nsec = ts[1];

        if (sec < 0 || nsec < 0 || nsec >= 1000000000LL)
            return ERR(EINVAL);

        uint64_t s = (uint64_t)sec;
        uint64_t ns_ticks =
            ((uint64_t)nsec + 9999999ULL) / 10000000ULL;
        uint64_t ticks;

        if (s > (UINT64_MAX - ns_ticks) / 100ULL)
            ticks = UINT64_MAX;
        else
            ticks = s * 100ULL + ns_ticks;

        uint64_t now = timer_ticks();
        deadline = UINT64_MAX - now < ticks
            ? UINT64_MAX : now + ticks;
    }

    /*
     * ABI pselect6: us apunta a { sigset_t *ss; size_t ss_len }.
     * Se acepta una mascara nula; aplicar una mascara no nula
     * queda pendiente para una implementación posterior.
     */
    if (us) {
        if (!user_range_ok(us, 16)) return ERR(EFAULT);
        const uint64_t *sm = (const uint64_t *)us;
        if (sm[1] != 8) return ERR(EINVAL);
    }

    for (;;) {
        uint64_t rout = 0, wout = 0, xout = 0;

        for (uint64_t fd = 0; fd < nfds; fd++) {
            uint64_t bit = 1ULL << fd;

            if (rin & bit) {
                /* La consola solo esta lista para leer con datos en cola. */
                if (fd == 0) {
                    if (keyboard_has_data()) rout |= bit;
                } else {
                    rout |= bit;
                }
            }

            /* Escritura: aproximacion actual para descriptores validos. */
            if (win & bit) wout |= bit;

            /* Las condiciones de excepcion aun no estan soportadas. */
        }

        uint64_t ready = 0;
        for (uint64_t fd = 0; fd < nfds; fd++) {
            uint64_t bit = 1ULL << fd;
            if ((rout | wout | xout) & bit) ready++;
        }

        if (ready || (has_timeout && timer_ticks() >= deadline)) {
            if (bytes && ur) *(uint64_t *)ur = rout;
            if (bytes && uw) *(uint64_t *)uw = wout;
            if (bytes && ux) *(uint64_t *)ux = xout;
            return ready;
        }

        if (task_sigpending()) return ERR(EINTR);

        yield();
        __asm__ volatile("sti; hlt; cli" : : : "memory");
    }
}

/* nanosleep: espera por ticks (100 Hz); EINTR si llega una senal */
static uint64_t sys_nanosleep(uint64_t req, uint64_t rem)
{
    (void)rem;
    if (!user_range_ok(req, 16)) return ERR(EFAULT);
    const uint64_t *t = (const uint64_t *)req;
    if (t[1] >= 1000000000ULL || t[0] > 100000000ULL) return ERR(EINVAL);
    uint64_t ticks = t[0] * 100 + (t[1] + 9999999ULL) / 10000000ULL;
    uint64_t end = timer_ticks() + ticks;
    while (timer_ticks() < end) {
        if (task_sigpending()) return ERR(4);
        yield();
        __asm__ volatile("sti; hlt; cli" : : : "memory");
    }
    return 0;
}

extern int user_wait2(int pid, int *status, int nohang, int untraced);

static uint64_t sys_wait4(uint64_t pid, uint64_t ust, uint64_t opt, uint64_t ru)
{
    (void)ru;
    if (opt & ~(1ULL | 2ULL | 8ULL)) return ERR(EINVAL);   /* WNOHANG; WUNTRACED y WCONTINUED se ignoran (aun no hay stop) */
    if (ust && !user_range_ok(ust, 4)) return ERR(EFAULT);
    int st = 0;
    int r = user_wait2((int)pid, &st, (int)(opt & 1), (int)((opt >> 1) & 1));
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

    int pe = check_path_parents(path);
    if (pe) return ERR(pe);

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

    struct file *xf = initramfs_open(path);
    if (!xf) return ERR(ENOENT);

    int exec_allowed = file_access_allowed(xf, 1, 0);
    file_put(xf);
    if (!exec_allowed) return ERR(EACCES);

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

static uint64_t sys_sigaction(uint64_t sig, uint64_t act, uint64_t old, uint64_t sz)
{
    struct proc *p = task_proc();
    if (sz != 8 || sig < 1 || sig >= NSIG) return ERR(EINVAL);
    if (act && (sig == SIGKILL || sig == SIGSTOP)) return ERR(EINVAL);
    if ((act && !user_range_ok(act, 32)) || (old && !user_range_ok(old, 32))) return ERR(EFAULT);
    struct ksigaction *sa = &p->sa[sig - 1];
    if (old) {
        uint64_t *o = (uint64_t *)old;
        o[0] = sa->handler; o[1] = sa->flags; o[2] = sa->restorer; o[3] = sa->mask;
    }
    if (act) {
        const uint64_t *a = (const uint64_t *)act;
        sa->handler = a[0]; sa->flags = a[1]; sa->restorer = a[2];
        sa->mask = a[3] & ~SIG_UNBLOCKABLE;
        if (sa->handler == SIG_IGN || (sa->handler == SIG_DFL && sig_default_ignored((int)sig)))
            p->sig_pending &= ~SIG_BIT(sig);
    }
    return 0;
}

static uint64_t sys_sigprocmask(uint64_t how, uint64_t set, uint64_t old, uint64_t sz)
{
    struct proc *p = task_proc();
    if (sz != 8) return ERR(EINVAL);
    if ((set && !user_range_ok(set, 8)) || (old && !user_range_ok(old, 8))) return ERR(EFAULT);
    uint64_t cur = p->sig_blocked;
    if (old) *(uint64_t *)old = cur;
    if (set) {
        uint64_t s = *(uint64_t *)set;
        switch (how) {
        case 0: cur |= s; break;
        case 1: cur &= ~s; break;
        case 2: cur = s; break;
        default: return ERR(EINVAL);
        }
        p->sig_blocked = cur & ~SIG_UNBLOCKABLE;
    }
    return 0;
}

static uint64_t sys_kill(uint64_t pid, uint64_t sig)
{
    if (sig >= NSIG) return ERR(EINVAL);
    int64_t p = (int32_t)pid;                      /* pid_t es de 32 bits con signo */
    if (p > 0) return task_kill((int)p, (int)sig) ? ERR(3) : 0;
    if (p == -1) return ERR(3);
    int pg = p == 0 ? task_pgid() : (int)-p;
    return task_kill_pgrp(pg, (int)sig) > 0 ? 0 : ERR(3);
}

static uint64_t sys_setpgid(uint64_t pid, uint64_t pg)
{
    if ((int64_t)pid < 0 || (int64_t)pg < 0) return ERR(EINVAL);
    int me = task_id();
    int target = pid ? (int)pid : me;
    int npg = pg ? (int)pg : target;
    if (target != me && user_parent_of(target) != me) return ERR(3);
    int rc = task_setpgid(target, npg);
    return rc == 0 ? 0 : rc == -1 ? ERR(3) : ERR(1);
}

static uint64_t sys_getpgid(uint64_t pid)
{
    int v = task_getpgid((int)pid);
    return v < 0 ? ERR(3) : (uint64_t)v;
}

static uint64_t sys_getsid(uint64_t pid)
{
    int v = task_getsid((int)pid);
    return v < 0 ? ERR(3) : (uint64_t)v;
}

static uint64_t sys_setsid(void)
{
    return task_setsid() ? ERR(1) : (uint64_t)task_id();
}


/* getresuid/getresgid: los punteros NULL son válidos; se validan
   todos los destinos antes de escribir para evitar resultados parciales. */
static uint64_t sys_getres(uint64_t a, uint64_t b, uint64_t c, int groups)
{
    struct proc *p = task_proc();
    uint32_t v[3];

    if (groups) {
        v[0] = p->rgid; v[1] = p->egid; v[2] = p->sgid;
    } else {
        v[0] = p->ruid; v[1] = p->euid; v[2] = p->suid;
    }

    uint64_t dst[3] = { a, b, c };
    for (int i = 0; i < 3; i++)
        if (dst[i] && !user_range_ok(dst[i], sizeof(uint32_t)))
            return ERR(14); /* EFAULT */

    for (int i = 0; i < 3; i++)
        if (dst[i])
            *(uint32_t *)dst[i] = v[i];

    return 0;
}

/* En las variantes setre* y setres*, -1 significa "no modificar".
   En x86-64 puede llegar como 0xffffffff o como UINT64_MAX. */
static int parse_optional_id(uint64_t raw, uint32_t *value)
{
    if (raw == UINT64_MAX || raw == UINT32_MAX)
        return 1;
    if (raw > UINT32_MAX)
        return -1;
    *value = (uint32_t)raw;
    return 0;
}

static int uid_permitted(const struct proc *p, uint32_t id)
{
    return p->euid == 0 ||
           id == p->ruid || id == p->euid || id == p->suid;
}

static int gid_permitted(const struct proc *p, uint32_t id)
{
    return p->euid == 0 ||
           id == p->rgid || id == p->egid || id == p->sgid;
}

static uint64_t sys_getuid(void)  { return task_proc()->ruid; }
static uint64_t sys_geteuid(void) { return task_proc()->euid; }
static uint64_t sys_getgid(void)  { return task_proc()->rgid; }
static uint64_t sys_getegid(void) { return task_proc()->egid; }

static uint64_t sys_setuid(uint64_t raw)
{
    struct proc *p = task_proc();
    if (raw > UINT32_MAX || raw == UINT32_MAX) return ERR(22); /* EINVAL */
    uint32_t id = (uint32_t)raw;

    if (p->euid == 0) {
        p->ruid = p->euid = p->suid = id;
        return 0;
    }
    if (!uid_permitted(p, id)) return ERR(1); /* EPERM */
    p->euid = id;
    return 0;
}

static uint64_t sys_setgid(uint64_t raw)
{
    struct proc *p = task_proc();
    if (raw > UINT32_MAX || raw == UINT32_MAX) return ERR(22);
    uint32_t id = (uint32_t)raw;

    if (p->euid == 0) {
        p->rgid = p->egid = p->sgid = id;
        return 0;
    }
    if (!gid_permitted(p, id)) return ERR(1);
    p->egid = id;
    return 0;
}

static uint64_t sys_setreuid(uint64_t ur, uint64_t ue)
{
    struct proc *p = task_proc();
    uint32_t r = 0, e = 0;
    int sr = parse_optional_id(ur, &r);
    int se = parse_optional_id(ue, &e);
    if (sr < 0 || se < 0) return ERR(22);

    if (p->euid != 0) {
        if (sr == 0 && r != p->ruid && r != p->euid) return ERR(1);
        if (se == 0 && e != p->ruid && e != p->euid && e != p->suid)
            return ERR(1);
    }

    if (sr == 0) p->ruid = r;
    if (se == 0) p->euid = e;
    if (sr == 0 || se == 0) p->suid = p->euid;
    return 0;
}

static uint64_t sys_setregid(uint64_t gr, uint64_t ge)
{
    struct proc *p = task_proc();
    uint32_t r = 0, e = 0;
    int sr = parse_optional_id(gr, &r);
    int se = parse_optional_id(ge, &e);
    if (sr < 0 || se < 0) return ERR(22);

    if (p->euid != 0) {
        if (sr == 0 && r != p->rgid && r != p->egid) return ERR(1);
        if (se == 0 && e != p->rgid && e != p->egid && e != p->sgid)
            return ERR(1);
    }

    if (sr == 0) p->rgid = r;
    if (se == 0) p->egid = e;
    if (sr == 0 || se == 0) p->sgid = p->egid;
    return 0;
}

static uint64_t sys_setresuid(uint64_t ur, uint64_t ue, uint64_t us)
{
    struct proc *p = task_proc();
    uint32_t id[3] = {0, 0, 0};
    int set[3];

    set[0] = parse_optional_id(ur, &id[0]);
    set[1] = parse_optional_id(ue, &id[1]);
    set[2] = parse_optional_id(us, &id[2]);
    if (set[0] < 0 || set[1] < 0 || set[2] < 0) return ERR(22);

    if (p->euid != 0) {
        for (int i = 0; i < 3; i++)
            if (set[i] == 0 && !uid_permitted(p, id[i])) return ERR(1);
    }

    if (set[0] == 0) p->ruid = id[0];
    if (set[1] == 0) p->euid = id[1];
    if (set[2] == 0) p->suid = id[2];
    return 0;
}

static uint64_t sys_setresgid(uint64_t gr, uint64_t ge, uint64_t gs)
{
    struct proc *p = task_proc();
    uint32_t id[3] = {0, 0, 0};
    int set[3];

    set[0] = parse_optional_id(gr, &id[0]);
    set[1] = parse_optional_id(ge, &id[1]);
    set[2] = parse_optional_id(gs, &id[2]);
    if (set[0] < 0 || set[1] < 0 || set[2] < 0) return ERR(22);

    if (p->euid != 0) {
        for (int i = 0; i < 3; i++)
            if (set[i] == 0 && !gid_permitted(p, id[i])) return ERR(1);
    }

    if (set[0] == 0) p->rgid = id[0];
    if (set[1] == 0) p->egid = id[1];
    if (set[2] == 0) p->sgid = id[2];
    return 0;
}

static uint64_t sys_umask(uint64_t value)
{
    struct proc *p = task_proc();
    uint32_t old = p->umask;
    p->umask = (uint32_t)value & 0777;
    return old;
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
    put_field(b + 65,  "nethra");
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
    if (mode & ~7ULL) return ERR(EINVAL);

    char p[PATH_MAX_K];
    int r = user_path(p, upath);
    if (r == -1) return ERR(EFAULT);
    if (r == -2) return ERR(ENAMETOOLONG);
    int pe = check_path_parents(p);
    if (pe) return ERR(pe);

    struct file *f = open_any(p);
    if (!f) return ERR(ENOENT);

    /* access(2) usa las credenciales reales, no las efectivas. */
    int allowed = file_access_allowed(f, (unsigned)mode, 1);
    file_put(f);

    return allowed ? 0 : ERR(EACCES);
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

static uint64_t sys_unlink(uint64_t upath)
{
    char p[PATH_MAX_K];
    int r = user_path(p, upath);
    if (r == -1) return ERR(EFAULT);
    if (r == -2) return ERR(ENAMETOOLONG);

    int pe = check_parent_access(p);
    if (pe) return ERR(pe);

    int rc = tmpfs_unlink(p, task_proc()->euid);
    if (rc == -2) {
        struct file *f = initramfs_open(p);
        if (f) { file_put(f); return ERR(EROFS); }
        return ERR(ENOENT);
    }
    return rc < 0 ? ERR(-rc) : 0;
}

/* sysinfo: struct de 112 bytes de x86_64. mem_unit en el offset 104 */
static uint64_t sys_sysinfo(uint64_t ui)
{
    if (!user_range_ok(ui, 112)) return ERR(EFAULT);
    uint8_t *b = (uint8_t *)ui;
    for (int i = 0; i < 112; i++) b[i] = 0;
    *(int64_t  *)(b + 0)   = (int64_t)(timer_ticks() / 100);   /* uptime */
    *(uint64_t *)(b + 32)  = pmm_total_pages();                /* totalram */
    *(uint64_t *)(b + 40)  = pmm_free_pages();                 /* freeram  */
    *(uint16_t *)(b + 80)  = 1;                                /* procs */
    *(uint32_t *)(b + 104) = 4096;                             /* mem_unit */
    return 0;
}

void syscall_dispatch(struct regs *r)
{
    switch (r->rax) {
    case SYS_READ:  r->rax = sys_read(r->rdi, r->rsi, r->rdx); break;
    case SYS_WRITE: r->rax = sys_write(r->rdi, r->rsi, r->rdx); break;
    case SYS_CLOSE: r->rax = sys_close(r->rdi); break;
    case SYS_OPEN:   r->rax = sys_open(r->rdi, r->rsi, r->rdx); break;
    case SYS_OPENAT: r->rax = sys_openat(r->rdi, r->rsi, r->rdx, r->r10); break;
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
    case SYS_EXECVE: {
        uint64_t rc = sys_execve(r->rdi, r->rsi, r->rdx, r);
        if ((int64_t)rc >= 0) sig_reset_on_exec(task_proc());
        r->rax = rc;
        break;
    }
    case 13:  r->rax = sys_sigaction(r->rdi, r->rsi, r->rdx, r->r10); break;
    case 14:  r->rax = sys_sigprocmask(r->rdi, r->rsi, r->rdx, r->r10); break;
    case 15:  sig_sigreturn(r); break;            /* no toca rax: restaura todo el frame */
    case 7:   r->rax = sys_poll(r->rdi, r->rsi, r->rdx); break;
    case 270: r->rax = sys_pselect6(r->rdi, r->rsi, r->rdx,
                                    r->r10, r->r8, r->r9); break;
    case 35:  r->rax = sys_nanosleep(r->rdi, r->rsi); break;
    case 230: r->rax = sys_nanosleep(r->rdx, r->r10); break;   /* clock_nanosleep(clk, flags, req, rem) */
    case 62:  r->rax = sys_kill(r->rdi, r->rsi); break;
    case 200: r->rax = sys_kill(r->rdi, r->rsi); break;   /* tkill */
    case 234: r->rax = sys_kill(r->rsi, r->rdx); break;   /* tgkill */
    case 102: r->rax = sys_getuid(); break;
    case 104: r->rax = sys_getgid(); break;
    case 107: r->rax = sys_geteuid(); break;
    case 108: r->rax = sys_getegid(); break;
    case 105: r->rax = sys_setuid(r->rdi); break;
    case 106: r->rax = sys_setgid(r->rdi); break;
    case 113: r->rax = sys_setreuid(r->rdi, r->rsi); break;
    case 114: r->rax = sys_setregid(r->rdi, r->rsi); break;
    case 117: r->rax = sys_setresuid(r->rdi, r->rsi, r->rdx); break;
    case 118: r->rax = sys_getres(r->rdi, r->rsi, r->rdx, 0); break;
    case 119: r->rax = sys_setresgid(r->rdi, r->rsi, r->rdx); break;
    case 120: r->rax = sys_getres(r->rdi, r->rsi, r->rdx, 1); break;
    case 95:  r->rax = sys_umask(r->rdi); break;
    case 96:  r->rax = sys_gettimeofday(r->rdi, r->rsi); break;
    case 63:  r->rax = sys_uname(r->rdi); break;
    case 111: r->rax = (uint64_t)task_pgid(); break;
    case 72:  r->rax = sys_fcntl(r->rdi, r->rsi, r->rdx); break;
    case 32:  r->rax = sys_dup(r->rdi); break;
    case 33:  r->rax = sys_dup2(r->rdi, r->rsi); break;
    case 89:  r->rax = sys_readlink(r->rdi, r->rsi, r->rdx); break;
    case 262: r->rax = sys_newfstatat(r->rdi, r->rsi, r->rdx, r->r10); break;
    case 21:  r->rax = sys_access(r->rdi, r->rsi); break;
    case 269: r->rax = sys_access(r->rsi, r->rdx); break;
    case 90:  r->rax = sys_chmod(r->rdi, r->rsi); break;
    case 91:  r->rax = sys_fchmod(r->rdi, r->rsi); break;
    case 92:  r->rax = sys_chown(r->rdi, r->rsi, r->rdx); break;
    case 93:  r->rax = sys_fchown(r->rdi, r->rsi, r->rdx); break;
    case 94:  r->rax = sys_chown(r->rdi, r->rsi, r->rdx); break; /* lchown */
    case 260: r->rax = sys_chown(r->rsi, r->rdx, r->r10); break; /* fchownat */
    case 268: r->rax = sys_chmod(r->rsi, r->rdx); break;         /* fchmodat */
    case 452: r->rax = sys_chmod(r->rsi, r->rdx); break;         /* fchmodat2 */
    case 109: r->rax = sys_setpgid(r->rdi, r->rsi); break;
    case 121: r->rax = sys_getpgid(r->rdi); break;
    case 112: r->rax = sys_setsid(); break;
    case 124: r->rax = sys_getsid(r->rdi); break;
    case 87:  r->rax = sys_unlink(r->rdi); break;
    case 263: r->rax = sys_unlink(r->rsi); break;   /* unlinkat: dirfd ignorado */
    case 99:  r->rax = sys_sysinfo(r->rdi); break;
    case SYS_REBOOT:
        r->rax = sys_reboot(r->rdi, r->rsi, r->rdx);
        break;
    case SYS_SYNC:
        /* No hay todavía almacenamiento persistente que vaciar. */
        r->rax = 0;
        break;
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
    case SYS_TIME:       r->rax = sys_time(r->rdi); break;
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
        default:
            console_puts("[kernel] syscall sin implementar: ");
            console_dec(r->rax);
            console_putc('\n');
        }
        r->rax = ERR(38);                  /* ENOSYS */
    }
}
