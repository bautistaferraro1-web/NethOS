#ifndef FILE_H
#define FILE_H
#include <stdint.h>

#ifndef EBADF
#define EBADF 9
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif

#define NFD 32

struct file;
struct file_ops {
    int64_t (*read)(struct file *f, void *buf, uint64_t n);
    int64_t (*write)(struct file *f, const void *buf, uint64_t n);
    int64_t (*ioctl)(struct file *f, uint64_t req, uint64_t arg);
    int64_t (*stat)(struct file *f, void *st144);   /* st ya en cero */
};

struct file {
    const struct file_ops *ops;
    int refcount;
    uint64_t off;
    void *priv;
};

extern struct file console_file;

struct file *file_get(struct file *f);
void         file_put(struct file *f);

void         fd_init_std(struct file **fds);
struct file *fd_lookup(struct file **fds, uint64_t fd);
int          fd_install(struct file **fds, struct file *f);
int          fd_close(struct file **fds, uint64_t fd);
void         fd_close_all(struct file **fds);
#endif
