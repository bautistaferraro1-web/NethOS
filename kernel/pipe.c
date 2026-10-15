#include <stdint.h>
#include "file.h"
#include "heap.h"
#include "sched.h"

#include "cpu.h"

#define PIPE_SZ 4096
#define EPIPE_K 32

struct pipe {
    uint8_t  buf[PIPE_SZ];
    uint32_t head, count;
    int readers, writers;
};

struct pend { struct pipe *p; int is_writer; };

static int64_t pipe_read(struct file *f, void *buf, uint64_t n)
{
    struct pend *e = f->priv;
    struct pipe *p = e->p;
    uint8_t *d = buf;
    if (n == 0) return 0;
    for (;;) {
        uint64_t fl = irq_save();
        if (p->count > 0) {
            uint64_t i = 0;
            while (i < n && p->count > 0) {
                d[i++] = p->buf[p->head];
                p->head = (p->head + 1) % PIPE_SZ;
                p->count--;
            }
            irq_restore(fl);
            return (int64_t)i;
        }
        int w = p->writers;
        irq_restore(fl);
        if (w == 0) return 0;            /* EOF */
        yield();
    }
}

static int64_t pipe_write(struct file *f, const void *buf, uint64_t n)
{
    struct pend *e = f->priv;
    struct pipe *p = e->p;
    const uint8_t *s = buf;
    uint64_t i = 0;
    while (i < n) {
        uint64_t fl = irq_save();
        if (p->readers == 0) { irq_restore(fl); return -EPIPE_K; }
        while (i < n && p->count < PIPE_SZ) {
            p->buf[(p->head + p->count) % PIPE_SZ] = s[i++];
            p->count++;
        }
        int full = (i < n);
        irq_restore(fl);
        if (full) yield();
    }
    return (int64_t)n;
}

static int64_t pipe_stat(struct file *f, void *st)
{
    (void)f;
    ((uint64_t *)st)[2] = 1;                 /* st_nlink */
    ((uint32_t *)st)[6] = 0010600;           /* st_mode = S_IFIFO | 0600 */
    return 0;
}

static int64_t pipe_close(struct file *f)
{
    struct pend *e = f->priv;
    struct pipe *p = e->p;
    uint64_t fl = irq_save();
    if (e->is_writer) p->writers--; else p->readers--;
    int dead = (p->readers <= 0 && p->writers <= 0);
    irq_restore(fl);
    if (dead) kfree(p);
    kfree(e);
    kfree(f);
    return 0;
}

static const struct file_ops pipe_ops = { pipe_read, pipe_write, 0, pipe_stat, 0, 0, pipe_close };

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = kmalloc(sizeof *p);
    struct file *a = kmalloc(sizeof *a), *b = kmalloc(sizeof *b);
    struct pend *ea = kmalloc(sizeof *ea), *eb = kmalloc(sizeof *eb);
    if (!p || !a || !b || !ea || !eb) {
        if (p) kfree(p);
        if (a) kfree(a);
        if (b) kfree(b);
        if (ea) kfree(ea);
        if (eb) kfree(eb);
        return -1;
    }
    p->head = p->count = 0;
    p->readers = p->writers = 1;
    ea->p = p; ea->is_writer = 0;
    eb->p = p; eb->is_writer = 1;
    a->ops = &pipe_ops; a->refcount = 1; a->off = 0; a->priv = ea;
    b->ops = &pipe_ops; b->refcount = 1; b->off = 0; b->priv = eb;
    *rd = a; *wr = b;
    return 0;
}
