#include "tmpfs.h"
#include "heap.h"
#include "cpu.h"
#include "initramfs.h"

#define T_MAX   64
#define T_POOL  64
#define T_NAME  96
#define T_MAXSZ (256ULL * 1024)

#define O_CREAT_  0100
#define O_EXCL_   0200
#define O_TRUNC_  01000
#define O_APPEND_ 02000
#define S_IFREG_  0100000
#define S_IFDIR_  0040000
#define DIR_PHASE (1ULL << 63)

struct tnode {
    char name[T_NAME];
    uint8_t *data;
    uint64_t size, cap;
    int used;
    uint32_t mode, uid, gid;
    uint8_t is_dir, shadow;
};

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


static uint64_t path_len(const char *s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static struct tnode *node_alloc(const char *path, int is_dir, int shadow,
                                uint32_t mode, uint32_t uid, uint32_t gid)
{
    uint64_t len = path_len(path);
    if (len >= T_NAME) return 0;

    for (int i = 0; i < T_MAX; i++) {
        struct tnode *t = &nodes[i];
        if (t->used) continue;

        for (uint64_t j = 0; j <= len; j++)
            t->name[j] = path[j];

        t->data = 0;
        t->size = 0;
        t->cap = 0;
        t->mode = mode;
        t->uid = uid;
        t->gid = gid;
        t->is_dir = (uint8_t)(is_dir != 0);
        t->shadow = (uint8_t)(shadow != 0);
        t->used = 1;
        return t;
    }
    return 0;
}

/* child tiene que ser hijo directo de parent, no un descendiente más lejano. */
static int direct_child(const char *parent, const char *child,
                        const char **basename)
{
    uint64_t plen = path_len(parent);
    const char *p = child;

    if (plen) {
        for (uint64_t i = 0; i < plen; i++)
            if (child[i] != parent[i]) return 0;
        if (child[plen] != '/') return 0;
        p = child + plen + 1;
    }

    if (!*p) return 0;
    for (const char *q = p; *q; q++)
        if (*q == '/') return 0;

    *basename = p;
    return 1;
}

static int descendant_path(const char *parent, const char *child)
{
    uint64_t plen = path_len(parent);
    if (!plen) return child[0] != 0;

    for (uint64_t i = 0; i < plen; i++)
        if (child[i] != parent[i]) return 0;

    return child[plen] == '/' && child[plen + 1] != 0;
}

/*
 * Registra un directorio del initramfs en el overlay.
 * Los nodos shadow representan entradas existentes e inmutables.
 */
static int ensure_shadow_dir(const char *path)
{
    struct tnode *old = find(path);
    if (old) return old->is_dir ? 0 : -20;

    struct file *base = initramfs_open(path);
    if (!base) return -2;

    uint8_t st[144] = {0};
    if (!base->ops || !base->ops->stat ||
        base->ops->stat(base, st) < 0) {
        file_put(base);
        return -5;
    }

    uint32_t mode = *(uint32_t *)(st + 24);
    uint32_t uid = *(uint32_t *)(st + 28);
    uint32_t gid = *(uint32_t *)(st + 32);
    file_put(base);

    if ((mode & 0170000u) != S_IFDIR_)
        return -20;

    if (path_len(path) >= T_NAME)
        return -36;

    return node_alloc(path, 1, 1, mode, uid, gid) ? 0 : -28;
}

static int ensure_parent_dirs(const char *path)
{
    if (path_len(path) >= T_NAME) return -36;

    int rc = ensure_shadow_dir("");
    if (rc < 0) return rc;

    char prefix[T_NAME];

    for (const char *q = path; *q; q++) {
        if (*q != '/') continue;

        uint64_t n = (uint64_t)(q - path);
        if (n >= sizeof(prefix)) return -36;

        for (uint64_t i = 0; i < n; i++) prefix[i] = path[i];
        prefix[n] = 0;

        rc = ensure_shadow_dir(prefix);
        if (rc < 0) return rc;
    }

    return 0;
}


static int64_t t_read(struct file *f, void *buf, uint64_t n)
{
    if ((pflags[f - pool] & 3) == 1) return -9;          /* O_WRONLY: EBADF */
    struct tnode *t = f->priv;
    if (t->is_dir) return -21; /* EISDIR */
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
    if (t->is_dir) return -21; /* EISDIR */
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
    *(uint64_t *)(b + 16) = t->is_dir ? 2 : 1;
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


static int64_t dirent_emit(uint8_t *out, uint64_t cap, uint64_t used,
                           uint64_t ino, uint64_t off, uint8_t type,
                           const char *name)
{
    uint64_t len = path_len(name);
    uint64_t reclen = (19 + len + 1 + 7) & ~7ULL;

    if (used + reclen > cap) return -22;

    uint8_t *r = out + used;
    *(uint64_t *)(r + 0) = ino;
    *(int64_t  *)(r + 8) = (int64_t)off;
    *(uint16_t *)(r + 16) = (uint16_t)reclen;
    r[18] = type;

    for (uint64_t i = 0; i <= len; i++)
        r[19 + i] = (uint8_t)name[i];

    return (int64_t)reclen;
}

static int64_t t_getdents(struct file *f, void *buf, uint64_t n)
{
    struct tnode *t = f->priv;
    if (!t || !t->used) return -9;
    if (!t->is_dir) return -20;
    if (n == 0) return -22;

    /*
     * Primero se enumeran las entradas del initramfs.
     * DIR_PHASE marca que esa parte ya terminó.
     */
    if (!(f->off & DIR_PHASE)) {
        struct file *base = initramfs_open(t->name);

        if (base) {
            if (!base->ops || !base->ops->getdents) {
                file_put(base);
                return -20;
            }

            base->off = f->off;
            int64_t rc = base->ops->getdents(base, buf, n);
            f->off = base->off;
            file_put(base);

            if (rc != 0) return rc;
        } else if (t->shadow) {
            return -2;
        }

        f->off = DIR_PHASE;
    }

    uint64_t cursor = f->off & ~DIR_PHASE;
    uint8_t *out = buf;
    uint64_t used = 0;

    for (;;) {
        if (!t->shadow && cursor < 2) {
            const char *name = cursor == 0 ? "." : "..";
            int64_t rec = dirent_emit(out, n, used, 1, cursor + 1, 4, name);

            if (rec < 0) {
                f->off = DIR_PHASE | cursor;
                return used ? (int64_t)used : rec;
            }

            used += (uint64_t)rec;
            cursor++;
            f->off = DIR_PHASE | cursor;
            continue;
        }

        uint64_t idx = t->shadow ? cursor : cursor - 2;
        if (idx >= T_MAX) {
            f->off = DIR_PHASE | cursor;
            return (int64_t)used;
        }

        struct tnode *child = &nodes[idx];
        const char *name = 0;

        if (!child->used || child->shadow || child == t ||
            !direct_child(t->name, child->name, &name)) {
            cursor++;
            f->off = DIR_PHASE | cursor;
            continue;
        }

        uint8_t type = child->is_dir ? 4 : 8;
        int64_t rec = dirent_emit(out, n, used, idx + 2,
                                  cursor + 1, type, name);

        if (rec < 0) {
            f->off = DIR_PHASE | cursor;
            return used ? (int64_t)used : rec;
        }

        used += (uint64_t)rec;
        cursor++;
        f->off = DIR_PHASE | cursor;
    }
}

static const struct file_ops t_ops = { t_read, t_write, 0, t_stat, t_lseek, t_getdents, 0 };

/* Devuelve 0 o el errno negativo. No modifica archivos de otros filesystems. */
static int tmpfs_fd_node(struct file *f, struct tnode **out)
{
    if (!f || !f->ops) return -9; /* EBADF */

    if (f->ops != &t_ops)
        return f->ops->stat ? -30 : -22; /* EROFS o EINVAL */

    struct tnode *t = f->priv;
    if (!t || !t->used) return -22; /* EINVAL */
    if (t->shadow) return -30;      /* EROFS */

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
        if (t->is_dir && (flags & (3 | O_CREAT_ | O_TRUNC_ | O_APPEND_))) {
            *err = 21; return 0; /* EISDIR */
        }
        if ((flags & O_TRUNC_) && (flags & 3)) t->size = 0;
    } else {
        if (!(flags & O_CREAT_)) { *err = 2; return 0; }                        /* ENOENT */
        int l = 0;
        while (path[l]) l++;
        if (!l) { *err = 21; return 0; }                 /* EISDIR */
        if (l >= T_NAME) { *err = 36; return 0; }        /* ENAMETOOLONG */
        for (int i = 0; i < T_MAX; i++) if (!nodes[i].used) { t = &nodes[i]; break; }
        if (!t) { *err = 28; return 0; }                 /* ENOSPC */
        int prc = ensure_parent_dirs(path);
        if (prc < 0) { *err = -prc; return 0; }
        t = node_alloc(path, 0, 0,
                       S_IFREG_ | ((mode & 0777u) & ~(mask & 0777u)),
                       uid, gid);
        if (!t) { *err = 28; return 0; } /* ENOSPC */
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
    if (!t) return -2;
    if (t->shadow) return -30; /* EROFS */                 /* ENOENT */

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
    if (!t) return -2;
    if (t->shadow) return -30; /* EROFS */                 /* ENOENT */
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


int tmpfs_mkdir_as(const char *raw, uint32_t mode,
                   uint32_t uid, uint32_t gid, uint32_t mask)
{
    const char *path = norm(raw);
    uint64_t len = path_len(path);

    if (!len) return -17;       /* EEXIST: raíz */
    if (len >= T_NAME) return -36;
    if (find(path)) return -17;

    struct file *base = initramfs_open(path);
    if (base) {
        file_put(base);
        return -17;             /* EEXIST */
    }

    int rc = ensure_parent_dirs(path);
    if (rc < 0) return rc;

    struct tnode *t = node_alloc(
        path, 1, 0,
        S_IFDIR_ | ((mode & 0777u) & ~(mask & 0777u)),
        uid, gid);

    return t ? 0 : -28;         /* ENOSPC */
}

int tmpfs_rmdir(const char *raw, uint32_t euid)
{
    const char *path = norm(raw);
    if (!*path) return -16;     /* EBUSY: raíz */

    struct tnode *t = find(path);

    if (!t) {
        struct file *base = initramfs_open(path);
        if (!base) return -2;

        uint8_t st[144] = {0};
        int ok = base->ops && base->ops->stat &&
                 base->ops->stat(base, st) == 0;
        uint32_t mode = ok ? *(uint32_t *)(st + 24) : 0;
        file_put(base);

        if (!ok) return -5;
        if ((mode & 0170000u) != S_IFDIR_) return -20;
        return -30;             /* EROFS */
    }

    if (!t->is_dir) return -20;
    if (t->shadow) return -30;
    if (euid != 0 && euid != t->uid) return -1; /* EPERM */

    for (int i = 0; i < T_MAX; i++) {
        if (nodes[i].used && &nodes[i] != t &&
            descendant_path(t->name, nodes[i].name))
            return -39;         /* ENOTEMPTY */
    }

    for (int i = 0; i < T_POOL; i++) {
        if (pool[i].refcount > 0 && pool[i].priv == t)
            return -16;         /* EBUSY */
    }

    t->used = 0;
    t->name[0] = 0;
    t->data = 0;
    t->size = 0;
    t->cap = 0;
    t->mode = 0;
    t->uid = 0;
    t->gid = 0;
    t->is_dir = 0;
    t->shadow = 0;
    return 0;
}

int tmpfs_unlink(const char *path, uint32_t euid)
{
    struct tnode *t = find(path);
    if (!t) return -2;
    if (t->is_dir) return -21;  /* EISDIR */
    if (t->shadow) return -30;  /* EROFS */
    if (euid != 0 && euid != t->uid) return -1; /* EPERM: política conservadora */
    for (int i = 0; i < T_POOL; i++)
        if (pool[i].refcount > 0 && pool[i].priv == t) return -16;   /* EBUSY */
    if (t->data) { uint64_t g = irq_save(); kfree(t->data); irq_restore(g); }
    t->data = 0; t->size = 0; t->cap = 0; t->used = 0; t->name[0] = 0;
    return 0;
}
