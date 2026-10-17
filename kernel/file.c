#include "file.h"
#include "console.h"
#include "keyboard.h"
#include "sched.h"
#include "user.h"

#define KTERMIOS_SIZE 36
#define TCGETS 0x5401
#define S_IFCHR_ 0020000

static int con_owner;
static int con_echo;                 /* 1 = modo cocinado: eco + linea completa */
static char con_line[256];
static int con_llen, con_lpos;

static int con_wait_key(void)
{
    int c;
    while ((c = keyboard_getc()) < 0) {
        if (task_sigpending()) return -4; /* EINTR */
        __asm__ volatile("sti; hlt; cli" : : : "memory");
    }
    return c;
}

static int64_t con_read(struct file *f, void *buf, uint64_t n)
{
    (void)f;
    if (n == 0) return 0;
    char *d = buf;
    uint64_t i = 0;

    if (con_echo) {
        if (con_lpos >= con_llen) {              /* armar una linea nueva */
            con_llen = con_lpos = 0;
            for (;;) {
                int c = con_wait_key();
                if (c == 0x08 || c == 0x7F) {
                    if (con_llen > 0) { con_llen--; console_puts("\b \b"); }
                } else if (c == '\n' || c == '\r') {
                    con_line[con_llen++] = '\n';
                    console_putc('\n');
                    break;
                } else if (con_llen < (int)sizeof(con_line) - 1) {
                    con_line[con_llen++] = (char)c;
                    console_putc((char)c);
                }
            }
        }
        while (i < n && con_lpos < con_llen) d[i++] = con_line[con_lpos++];
        return (int64_t)i;
    }

    int c = con_wait_key();                      /* modo crudo: como antes */
    d[i++] = (char)c;
    while (i < n && (c = keyboard_getc()) >= 0) d[i++] = (char)c;
    return (int64_t)i;
}

static int64_t con_write(struct file *f, const void *buf, uint64_t n)
{
    (void)f;
    const char *s = buf;
    for (uint64_t i = 0; i < n; i++) console_putc(s[i]);
    return (int64_t)n;
}

static int64_t con_ioctl(struct file *f, uint64_t req, uint64_t arg)
{
    (void)f;
    if (req == TCGETS) {
        for (int i = 0; i < KTERMIOS_SIZE; i++) ((uint8_t *)arg)[i] = 0;
        return 0;
    }
    if (req == 0x802C542A) {                       /* TCGETS2: termios2, 44 bytes */
        for (int i = 0; i < 44; i++) ((uint8_t *)arg)[i] = 0;
        return 0;
    }
    if (req == 0x540F) { *(int *)arg = task_id(); return 0; }   /* TIOCGPGRP */
    if (req == 0x5410) return 0;                                /* TIOCSPGRP */
    if (req == 0x5F01) { con_owner = task_id(); con_echo = (int)arg; con_llen = con_lpos = 0; return 0; }   /* propio */
    return -EINVAL;
}

static int64_t con_stat(struct file *f, void *st)
{
    (void)f;
    uint8_t *b = st;
    *(uint64_t *)(b + 16) = 1;
    *(uint32_t *)(b + 24) = S_IFCHR_ | 0620;
    *(uint64_t *)(b + 40) = (136ULL << 8);
    *(int64_t  *)(b + 56) = 1024;
    return 0;
}

static const struct file_ops con_ops = { con_read, con_write, con_ioctl, con_stat, 0, 0, 0 };
struct file console_file = { &con_ops, 1, 0, 0 };

void console_cooked_release(void) { if (con_echo && con_owner == task_id()) con_echo = 0; }

struct file *file_get(struct file *f) { if (f) f->refcount++; return f; }

void file_put(struct file *f)
{
    if (!f) return;
    if (--f->refcount <= 0 && f != &console_file && f->ops->close) f->ops->close(f);
}

void fd_init_std(struct file **fds)
{
    con_echo = 0;
    for (int i = 0; i < NFD; i++) fds[i] = 0;
    for (int i = 0; i < 3; i++) fds[i] = file_get(&console_file);
}

struct file *fd_lookup(struct file **fds, uint64_t fd) { return fd < NFD ? fds[fd] : 0; }

int fd_install(struct file **fds, struct file *f)
{
    for (int i = 0; i < NFD; i++) if (!fds[i]) { fds[i] = f; return i; }
    return -1;
}

int fd_close(struct file **fds, uint64_t fd)
{
    if (fd >= NFD || !fds[fd]) return -EBADF;
    file_put(fds[fd]);
    fds[fd] = 0;
    return 0;
}

void fd_close_all(struct file **fds)
{
    for (int i = 0; i < NFD; i++) if (fds[i]) fd_close(fds, i);
}
