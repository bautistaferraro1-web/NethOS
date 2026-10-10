#include "initramfs.h"

extern const uint8_t _binary_initramfs_cpio_start[], _binary_initramfs_cpio_end[];

#define MAX_ENT  128
#define POOL_SZ  64
#define S_IFREG_ 0100000

struct ent { const char *name; const uint8_t *data; uint64_t size; uint32_t mode; };
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
    ents[nents++] = (struct ent){ "", 0, 0, 0040755 };   /* raiz sintetica */
    while (p + 110 <= end && nents < MAX_ENT) {
        if (p[0] != '0' || p[1] != '7' || p[2] != '0' || p[3] != '7' || p[4] != '0' || p[5] != '1') break;
        uint32_t mode = hex8(p + 14), fsz = hex8(p + 54), nsz = hex8(p + 94);
        const char *name = (const char *)p + 110;
        const uint8_t *data = (const uint8_t *)(((uint64_t)(p + 110 + nsz) + 3) & ~3ULL);
        if (streq(name, "TRAILER!!!")) break;
        while (name[0] == '.' && name[1] == '/') name += 2;
        ents[nents++] = (struct ent){ name, data, fsz, mode };
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

static const struct file_ops rf_ops = { rf_read, 0, 0, rf_stat, rf_lseek };

struct file *initramfs_open(const char *path)
{
    if (!inited) { parse(); inited = 1; }
    for (;;) {
        if (path[0] == '/') path++;
        else if (path[0] == '.' && path[1] == '/') path += 2;
        else break;
    }
    struct ent *e = 0;
    for (int i = 0; i < nents; i++) if (streq(ents[i].name, path)) { e = &ents[i]; break; }
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
