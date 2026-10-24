#include "tmpfs.h"
#include "heap.h"
#include "cpu.h"

#define T_MAX   32
#define T_POOL  64
#define T_NAME  96
#define T_MAXSZ (256ULL * 1024)

#define O_CREAT_  0100
#define O_EXCL_   0200
#define O_TRUNC_  01000
#define O_APPEND_ 02000
#define S_IFREG_  0100000

struct tnode { char name[T_NAME]; uint8_t *data; uint64_t size, cap; int used; uint32_t mode, uid, gid; };

static struct tnode nodes[T_MAX];
static struct file  pool[T_POOL];
static int          pflags[T_POOL];

static const char *norm(const char *p) { while (*p == '/') p++; return p; }

static int seq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static struct tnode *find(const char *path)
{
    path = norm(path);
    for (int i = 0; i < T_MAX; i++)
        if (nodes[i].used && seq(nodes[i].name, path)) return &nodes[i];
    return 0;
}

int tmpfs_exists(const char *path) { return find(path) != 0; }

static int64_t t_read(struct file *f, void *buf, uint64_t n)
{
    if ((pflags[f - pool] & 3) == 1) return -9;          /* O_WRONLY: EBADF */
    struct tnode *t = f->priv;
    if (f->off >= t->size) return 0;
    if (n > t->size - f->off) n = t->size - f->off;
    uint8_t *d = buf;
    for (uint64_t i = 0; i < n; i++) d[i] = t->data[f->off + i];
    f->off += n;
    return (int64_t)n;
}

static int64_t t_write(struct file *f, const void *buf, uint64_t n)
{
    int fl = pflags[f - pool];
    if ((fl & 3) == 0) return -9;                        /* O_RDONLY: EBADF */
    struct tnode *t = f->priv;
    if (fl & O_APPEND_) f->off = t->size;
    if (n == 0) return 0;
    if (f->off > T_MAXSZ || n > T_MAXSZ - f->off) return -27;   /* EFBIG */
    uint64_t end = f->off + n;
    if (end > t->cap) {
        uint64_t nc = t->cap ? t->cap : 256;
        while (nc < end) nc *= 2;
        if (nc > T_MAXSZ) nc = T_MAXSZ;
        uint64_t g = irq_save();
        uint8_t *nd = kmalloc(nc);
        irq_restore(g);
        if (!nd) return -12;                             /* ENOMEM */
        for (uint64_t i = 0; i < t->size; i++) nd[i] = t->data[i];
        if (t->data) { g = irq_save(); kfree(t->data); irq_restore(g); }
        t->data = nd;
        t->cap = nc;
    }
    for (uint64_t i = t->size; i < f->off; i++) t->data[i] = 0;   /* hueco tras lseek */
    const uint8_t *s = buf;
    for (uint64_t i = 0; i < n; i++) t->data[f->off + i] = s[i];
    f->off = end;
    if (end > t->size) t->size = end;
    return (int64_t)n;
}

static int64_t t_stat(struct file *f, void *st)
{
    struct tnode *t = f->priv;
    uint8_t *b = st;
    *(uint64_t *)(b + 16) = 1;
    *(uint32_t *)(b + 24) = t->mode;
    *(uint32_t *)(b + 28) = t->uid;
    *(uint32_t *)(b + 32) = t->gid;
    *(int64_t  *)(b + 48) = (int64_t)t->size;
    *(int64_t  *)(b + 56) = 4096;
    *(int64_t  *)(b + 64) = (int64_t)((t->size + 511) / 512);
    return 0;
}

static int64_t t_lseek(struct file *f, int64_t off, int whence)
{
    struct tnode *t = f->priv;
    int64_t base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = (int64_t)f->off;
    else if (whence == 2) base = (int64_t)t->size;
    else return -22;
    if (base + off < 0) return -22;
    f->off = (uint64_t)(base + off);
    return base + off;
}

static const struct file_ops t_ops = { t_read, t_write, 0, t_stat, t_lseek, 0, 0 };

/* Devuelve 0 o el errno negativo. No modifica archivos de otros filesystems. */
static int tmpfs_fd_node(struct file *f, struct tnode **out)
{
    if (!f || !f->ops) return -9; /* EBADF */

    if (f->ops != &t_ops)
        return f->ops->stat ? -30 : -22; /* EROFS o EINVAL */

    struct tnode *t = f->priv;
    if (!t || !t->used) return -22; /* EINVAL */

    *out = t;
    return 0;
}

int tmpfs_fchmod(struct file *f, uint32_t mode, uint32_t euid)
{
    struct tnode *t;
    int rc = tmpfs_fd_node(f, &t);
    if (rc < 0) return rc;

    if (euid != 0 && euid != t->uid)
        return -1; /* EPERM */

    uint32_t bits = mode & 07777u;
    if (euid != 0)
        bits &= 0777u;

    t->mode = (t->mode & 0170000u) | bits;
    return 0;
}

int tmpfs_fchown(struct file *f, uint32_t uid, uint32_t gid,
                 uint32_t euid)
{
    struct tnode *t;
    int rc = tmpfs_fd_node(f, &t);
    if (rc < 0) return rc;

    if (euid != 0)
        return -1; /* EPERM: política actual sin grupos suplementarios */

    int changed = (uid != UINT32_MAX && uid != t->uid) ||
                  (gid != UINT32_MAX && gid != t->gid);

    if (uid != UINT32_MAX) t->uid = uid;
    if (gid != UINT32_MAX) t->gid = gid;

    if (changed)
        t->mode &= ~06000u;

    return 0;
}

struct file *tmpfs_open_as(const char *path, int flags,
                            uint32_t mode, uint32_t uid, uint32_t gid,
                            uint32_t mask, int *err)
{
    path = norm(path);
    int idx = -1;
    for (int i = 0; i < T_POOL; i++) if (pool[i].refcount <= 0) { idx = i; break; }
    if (idx < 0) { *err = 23; return 0; }                /* ENFILE */

    struct tnode *t = find(path);
    if (t) {
        if ((flags & O_CREAT_) && (flags & O_EXCL_)) { *err = 17; return 0; }   /* EEXIST */
        if ((flags & O_TRUNC_) && (flags & 3)) t->size = 0;
    } else {
        if (!(flags & O_CREAT_)) { *err = 2; return 0; }                        /* ENOENT */
        int l = 0;
        while (path[l]) l++;
        if (!l) { *err = 21; return 0; }                 /* EISDIR */
        if (l >= T_NAME) { *err = 36; return 0; }        /* ENAMETOOLONG */
        for (int i = 0; i < T_MAX; i++) if (!nodes[i].used) { t = &nodes[i]; break; }
        if (!t) { *err = 28; return 0; }                 /* ENOSPC */
        for (int i = 0; i <= l; i++) t->name[i] = path[i];
        t->data = 0; t->size = 0; t->cap = 0; t->used = 1;
        t->mode = S_IFREG_ | ((mode & 0777u) & ~(mask & 0777u));
        t->uid = uid;
        t->gid = gid;
    }
    pool[idx].ops = &t_ops;
    pool[idx].refcount = 1;
    pool[idx].off = 0;
    pool[idx].priv = t;
    pflags[idx] = flags;
    return &pool[idx];
}

/* Compatibilidad para búsquedas y llamadas internas que no crean archivos. */
struct file *tmpfs_open(const char *path, int flags, int *err)
{
    return tmpfs_open_as(path, flags, 0644, 0, 0, 022, err);
}

int tmpfs_chmod(const char *path, uint32_t mode, uint32_t euid)
{
    struct tnode *t = find(path);
    if (!t) return -2;                 /* ENOENT */

    if (euid != 0 && euid != t->uid)
        return -1;                     /* EPERM */

    uint32_t bits = mode & 07777u;

    /* Un usuario no privilegiado no puede establecer bits setuid/setgid
       mediante esta implementación inicial. */
    if (euid != 0)
        bits &= 0777u;

    t->mode = (t->mode & 0170000u) | bits;
    return 0;
}

int tmpfs_chown(const char *path, uint32_t uid, uint32_t gid,
                uint32_t euid)
{
    struct tnode *t = find(path);
    if (!t) return -2;                 /* ENOENT */
    if (euid != 0) return -1;          /* EPERM */

    /* UINT32_MAX representa el argumento -1: conservar el identificador. */
    int change_uid = uid != UINT32_MAX && uid != t->uid;
    int change_gid = gid != UINT32_MAX && gid != t->gid;

    if (uid != UINT32_MAX) t->uid = uid;
    if (gid != UINT32_MAX) t->gid = gid;

    /* Cambiar propietario/grupo elimina los bits setuid y setgid. */
    if (change_uid || change_gid)
        t->mode &= ~06000u;

    return 0;
}

int tmpfs_unlink(const char *path, uint32_t euid)
{
    struct tnode *t = find(path);
    if (!t) return -2;
    if (euid != 0 && euid != t->uid) return -1; /* EPERM: política conservadora */
    for (int i = 0; i < T_POOL; i++)
        if (pool[i].refcount > 0 && pool[i].priv == t) return -16;   /* EBUSY */
    if (t->data) { uint64_t g = irq_save(); kfree(t->data); irq_restore(g); }
    t->data = 0; t->size = 0; t->cap = 0; t->used = 0; t->name[0] = 0;
    return 0;
}
