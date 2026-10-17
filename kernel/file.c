#include "file.h"
#include "console.h"
#include "keyboard.h"
#include "sched.h"
#include "user.h"

#define KTERMIOS_SIZE 36
#define TCGETS 0x5401
#define S_IFCHR_ 0020000

static int con_owner;
static int con_fgpgrp;
/* termios del kernel de Linux (36 bytes): 4 flags, c_line, c_cc[19] */
#define LF_ISIG   0x0001u
#define LF_ICANON 0x0002u
#define LF_ECHO   0x0008u
#define LF_ECHOE  0x0010u
static struct {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line;
    uint8_t  cc[19];
} __attribute__((packed)) con_tio;

#define con_echo (con_tio.lflag & LF_ICANON)     /* modo cocinado = ICANON */

static void con_tio_reset(void)
{
    uint8_t *p = (uint8_t *)&con_tio;
    for (unsigned i = 0; i < sizeof con_tio; i++) p[i] = 0;
    con_tio.iflag = 0x0500;                      /* ICRNL | IXON */
    con_tio.oflag = 0x0005;                      /* OPOST | ONLCR */
    con_tio.cflag = 0x00BF;                      /* B38400 | CS8 | CREAD */
    con_tio.lflag = LF_ISIG;                     /* crudo hasta que alguien pida cocinado */
    con_tio.cc[0] = 3;      /* VINTR  */
    con_tio.cc[1] = 0x1C;   /* VQUIT  */
    con_tio.cc[2] = 0x7F;   /* VERASE */
    con_tio.cc[3] = 0x15;   /* VKILL  */
    con_tio.cc[4] = 4;      /* VEOF   */
    con_tio.cc[6] = 1;      /* VMIN   */
}
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
                if (c < 0) { con_llen = con_lpos = 0; return c; }   /* EINTR */
                if (c == 0x08 || c == 0x7F) {
                    if (con_llen > 0) { con_llen--; if (con_tio.lflag & LF_ECHO) console_puts("\b \b"); }
                } else if (c == 4) {
                    if (con_llen == 0) return 0;
                    break;
                } else if (c == '\n' || c == '\r') {
                    con_line[con_llen++] = '\n';
                    if (con_tio.lflag & LF_ECHO) console_putc('\n');
                    break;
                } else if (con_llen < (int)sizeof(con_line) - 1) {
                    con_line[con_llen++] = (char)c;
                    if (con_tio.lflag & LF_ECHO) console_putc((char)c);
                }
            }
        }
        while (i < n && con_lpos < con_llen) d[i++] = con_line[con_lpos++];
        return (int64_t)i;
    }

    int c = con_wait_key();
    if (c < 0) return c;                         /* EINTR */
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
    if (req >= 0x402C542B && req <= 0x402C542D) {  /* TCSETS2 / TCSETSW2 / TCSETSF2 */
        uint8_t *d = (uint8_t *)&con_tio;
        for (int i = 0; i < KTERMIOS_SIZE; i++) d[i] = ((const uint8_t *)arg)[i];
        return 0;
    }
    if (req == 0x540B || req == 0x5409 || req == 0x540A) return 0;           /* TCFLSH / TCSBRK / TCXONC */
    if (req == TCGETS) {
        const uint8_t *s = (const uint8_t *)&con_tio;
        for (int i = 0; i < KTERMIOS_SIZE; i++) ((uint8_t *)arg)[i] = s[i];
        return 0;
    }
    if (req == 0x5402 || req == 0x5403 || req == 0x5404) {   /* TCSETS / TCSETSW / TCSETSF */
        uint8_t *d = (uint8_t *)&con_tio;
        for (int i = 0; i < KTERMIOS_SIZE; i++) d[i] = ((const uint8_t *)arg)[i];
        return 0;
    }
    if (req == 0x5413) {                                     /* TIOCGWINSZ: texto VGA 80x25 */
        uint16_t *w = (uint16_t *)arg;
        w[0] = 25; w[1] = 80; w[2] = 0; w[3] = 0;
        return 0;
    }
    if (req == 0x802C542A) {                       /* TCGETS2: termios2, 44 bytes */
        for (int i = 0; i < 44; i++) ((uint8_t *)arg)[i] = 0;
        return 0;
    }
    if (req == 0x540F) { *(int *)arg = con_fgpgrp ? con_fgpgrp : task_pgid(); return 0; }
    if (req == 0x5410) {
        int pg = *(const int *)arg;
        if (pg <= 0 || task_kill_pgrp(pg, 0) <= 0) return -1;
        con_fgpgrp = pg;
        return 0;
    }
    if (req == 0x5F01) {                           /* propio: 1 = cocinado, 0 = crudo */
        con_owner = task_id();
        con_fgpgrp = task_pgid();
        if (arg) con_tio.lflag |= (LF_ICANON | LF_ECHO | LF_ECHOE);
        else     con_tio.lflag &= ~(uint32_t)(LF_ICANON | LF_ECHO | LF_ECHOE);
        con_llen = con_lpos = 0;
        return 0;
    }
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

void console_cooked_release(void) { if (con_echo && con_owner == task_id()) { con_tio_reset(); con_fgpgrp = 0; } }

struct file *file_get(struct file *f) { if (f) f->refcount++; return f; }

void file_put(struct file *f)
{
    if (!f) return;
    if (--f->refcount <= 0 && f != &console_file && f->ops->close) f->ops->close(f);
}

void fd_init_std(struct file **fds)
{
    con_tio_reset();
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

int console_isig(void) { return (con_tio.lflag & LF_ISIG) != 0; }
int console_fg_pgrp(void) { return con_fgpgrp; }
