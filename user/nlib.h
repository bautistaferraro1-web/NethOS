#ifndef NLIB_H
#define NLIB_H
/* Helpers de usuario de Nethel: sin libc, syscalls por int 0x80 (ABI Linux) */
typedef unsigned long u64;

static inline long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}

static inline long sys_read(int fd, void *b, u64 n)        { return sys3(0, fd, (long)b, (long)n); }
static inline long sys_write(int fd, const void *b, u64 n) { return sys3(1, fd, (long)b, (long)n); }
struct nethel_iovec { const void *base; u64 len; };
static inline long sys_ioctl(int fd, u64 req, void *arg)   { return sys3(16, fd, (long)req, (long)arg); }
static inline long sys_writev(int fd, const struct nethel_iovec *iov, u64 n) { return sys3(20, fd, (long)iov, (long)n); }
static inline void sys_exit(long code)                     { sys3(60, code, 0, 0); for (;;) ; }

static inline u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static inline void print(const char *s) { sys_write(1, s, slen(s)); }

static inline int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline void print_num(u64 v)
{
    char t[21], o[22];
    int n = 0, i = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) o[i++] = t[--n];
    o[i] = 0;
    print(o);
}
/* Con NLIB_MAIN, _start arma (argc, argv, envp) del stack inicial y llama a
   int nmain(int argc, char **argv). Si nmain retorna, hace exit(codigo). */
#ifdef NLIB_MAIN
__asm__(
    ".pushsection .text\n"
    ".globl _start\n"
    "_start:\n"
    "    xor  %ebp, %ebp\n"
    "    mov  (%rsp), %rdi\n"
    "    lea  8(%rsp), %rsi\n"
    "    and  $-16, %rsp\n"
    "    call nmain\n"
    "    mov  %eax, %edi\n"
    "    mov  $60, %eax\n"
    "    int  $0x80\n"
    ".popsection\n");
#endif

#endif
