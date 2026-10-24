#include "initramfs.h"

extern const uint8_t _binary_initramfs_cpio_start[], _binary_initramfs_cpio_end[];

#define MAX_ENT  128
#define POOL_SZ  64
#define S_IFREG_ 0100000

struct ent { const char *name; const uint8_t *data; uint64_t size; uint32_t mode, uid, gid; };
static struct ent ents[MAX_ENT];
static int nents, inited;
static struct file pool[POOL_SZ];

static uint32_t hex8(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = p[i];
        v = (v << 4) | (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void parse(void)
{
    const uint8_t *p = _binary_initramfs_cpio_start, *end = _binary_initramfs_cpio_end;
    ents[nents++] = (struct ent){ "", 0, 0, 0040755, 0, 0 };   /* raiz sintetica */
    while (p + 110 <= end && nents < MAX_ENT) {
        if (p[0] != '0' || p[1] != '7' || p[2] != '0' || p[3] != '7' || p[4] != '0' || p[5] != '1') break;
        uint32_t mode = hex8(p + 14), uid = hex8(p + 22), gid = hex8(p + 30), fsz = hex8(p + 54), nsz = hex8(p + 94);
        const char *name = (const char *)p + 110;
        const uint8_t *data = (const uint8_t *)(((uint64_t)(p + 110 + nsz) + 3) & ~3ULL);
        if (streq(name, "TRAILER!!!")) break;
        while (name[0] == '.' && name[1] == '/') name += 2;
        ents[nents++] = (struct ent){ name, data, fsz, mode, uid, gid };
        p = (const uint8_t *)(((uint64_t)(data + fsz) + 3) & ~3ULL);
    }
}

static int64_t rf_read(struct file *f, void *buf, uint64_t n)
{
    struct ent *e = f->priv;
    if ((e->mode & 0170000) == 0040000) return -21;   /* EISDIR */
    if (f->off >= e->size) return 0;
    if (n > e->size - f->off) n = e->size - f->off;
    uint8_t *d = buf;
    for (uint64_t i = 0; i < n; i++) d[i] = e->data[f->off + i];
    f->off += n;
    return (int64_t)n;
}

static int64_t rf_stat(struct file *f, void *st)
{
    struct ent *e = f->priv;
    uint8_t *b = st;
    *(uint64_t *)(b + 16) = 1;                         /* st_nlink   */
    *(uint32_t *)(b + 24) = e->mode ? e->mode : (S_IFREG_ | 0644);
    *(uint32_t *)(b + 28) = e->uid;
    *(uint32_t *)(b + 32) = e->gid;
    *(int64_t  *)(b + 48) = (int64_t)e->size;          /* st_size    */
    *(int64_t  *)(b + 56) = 4096;                      /* st_blksize */
    *(int64_t  *)(b + 64) = (int64_t)((e->size + 511) / 512);
    return 0;
}

static int64_t rf_lseek(struct file *f, int64_t off, int whence)
{
    struct ent *e = f->priv;
    int64_t base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = (int64_t)f->off;
    else if (whence == 2) base = (int64_t)e->size;
    else return -EINVAL;
    if (base + off < 0) return -EINVAL;
    f->off = (uint64_t)(base + off);
    return base + off;
}

static int is_dir_ent(const struct ent *e) { return (e->mode & 0170000) == 0040000; }

/* cursor f->off: 0 ".", 1 "..", 2+i = ents[i] */
static int64_t rf_getdents(struct file *f, void *buf, uint64_t n)
{
    struct ent *d = f->priv;
    if (!is_dir_ent(d)) return -20;                     /* ENOTDIR */
    uint64_t nlen = 0;
    while (d->name[nlen]) nlen++;
    uint8_t *out = buf;
    uint64_t used = 0;
    for (uint64_t cur = f->off; cur < 2 + (uint64_t)nents; cur++) {
        const char *nm; uint8_t type; uint64_t ino;
        if (cur < 2) { nm = cur ? ".." : "."; type = 4; ino = 1; }
        else {
            struct ent *ch = &ents[cur - 2];
            const char *cn = ch->name;
            if (ch == d || !cn[0]) { f->off = cur + 1; continue; }
            if (nlen) {
                uint64_t k = 0;
                while (k < nlen && cn[k] == d->name[k]) k++;
                if (k != nlen || cn[nlen] != '/') { f->off = cur + 1; continue; }
                nm = cn + nlen + 1;
            } else nm = cn;
            int slash = !nm[0];
            for (const char *q = nm; *q; q++) if (*q == '/') slash = 1;
            if (slash) { f->off = cur + 1; continue; }
            type = is_dir_ent(ch) ? 4 : 8;
            ino = cur;
        }
        uint64_t l = 0;
        while (nm[l]) l++;
        uint64_t reclen = (19 + l + 1 + 7) & ~7ULL;
        if (used + reclen > n) { if (!used) return -22; break; }
        uint8_t *r = out + used;
        *(uint64_t *)r = ino;
        *(int64_t  *)(r + 8) = (int64_t)(cur + 1);
        *(uint16_t *)(r + 16) = (uint16_t)reclen;
        r[18] = type;
        for (uint64_t i = 0; i <= l; i++) r[19 + i] = (uint8_t)nm[i];
        used += reclen;
        f->off = cur + 1;
    }
    return (int64_t)used;
}

static int is_lnk_ent(const struct ent *e) { return (e->mode & 0170000) == 0120000; }

static struct ent *find_ent(const char *path)
{
    for (int i = 0; i < nents; i++) if (streq(ents[i].name, path)) return &ents[i];
    return 0;
}

static const struct file_ops rf_ops = { rf_read, 0, 0, rf_stat, rf_lseek, rf_getdents, 0 };

struct file *initramfs_open(const char *path)
{
    if (!inited) { parse(); inited = 1; }
    for (;;) {
        if (path[0] == '/') path++;
        else if (path[0] == '.' && path[1] == '/') path += 2;
        else break;
    }
    struct ent *e = find_ent(path);
    for (int depth = 0; e && is_lnk_ent(e); depth++) {      /* seguir symlinks (max 8) */
        if (depth >= 8 || e->size == 0 || e->size >= 128) return 0;
        char tgt[128], full[256];
        for (uint64_t i = 0; i < e->size; i++) tgt[i] = (char)e->data[i];
        tgt[e->size] = 0;
        if (tgt[0] == '/') {
            const char *t = tgt;
            while (*t == '/') t++;
            int n = 0;
            while (t[n]) { full[n] = t[n]; n++; }
            full[n] = 0;
        } else {
            int dl = 0, last = -1;
            while (e->name[dl]) { if (e->name[dl] == '/') last = dl; dl++; }
            int n = 0;
            for (int i = 0; i <= last; i++) full[n++] = e->name[i];
            for (int i = 0; tgt[i]; i++) full[n++] = tgt[i];
            full[n] = 0;
        }
        e = find_ent(full);
    }
    if (!e) return 0;
    for (int i = 0; i < POOL_SZ; i++) {
        if (pool[i].refcount <= 0) {
            pool[i].ops = &rf_ops; pool[i].refcount = 1; pool[i].off = 0; pool[i].priv = e;
            return &pool[i];
        }
    }
    return 0;
}

int initramfs_isdir(const char *path)
{
    struct file *f = initramfs_open(path);
    if (!f) return -1;
    int d = (((struct ent *)f->priv)->mode & 0170000) == 0040000;
    file_put(f);
    return d;
}

int initramfs_is_regular_nofollow(const char *path)
{
    if (!inited) { parse(); inited = 1; }
    if (!path) return -1;
    while (path[0] == '/') path++;
    while (path[0] == '.' && path[1] == '/') path += 2;
    struct ent *e = find_ent(path);
    if (!e) return -1;
    return (e->mode & 0170000u) == 0100000u;
}


/* 0 ok; -1 no existe; -2 no es archivo regular */
int initramfs_lookup(const char *path, const uint8_t **data, uint64_t *size)
{
    struct file *f = initramfs_open(path);
    if (!f) return -1;
    struct ent *e = f->priv;
    int r = 0;
    if ((e->mode & 0170000) == 0040000) r = -2;
    else { *data = e->data; *size = e->size; }
    file_put(f);
    return r;
}
