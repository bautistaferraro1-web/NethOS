/* Programa de usuario de Nethel: sin libc, solo syscalls por int 0x80 */
typedef unsigned long u64;

static inline long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}

static volatile int  counter = 3;       /* va a .data */
static volatile char zeros[256];        /* va a .bss  */

static void print(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    sys3(1, 1, (long)s, (long)n);
}

static void print_num(u64 v)
{
    char t[21], o[22];
    int n = 0, i = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) o[i++] = t[--n];
    o[i] = 0;
    print(o);
}

void _start(void)
{
    print("[hello.elf] cargado como ELF desde C\n");

    int bss_ok = 1;
    for (int i = 0; i < 256; i++) if (zeros[i]) bss_ok = 0;
    print(".data = "); print_num((u64)counter);
    print(bss_ok ? "  .bss en cero: OK\n" : "  .bss: FALLO\n");

    for (int i = 1; i <= 3; i++) {
        print("[hello.elf] vuelta "); print_num((u64)i); print("\n");
        sys3(24, 0, 0, 0);              /* yield */
    }
    sys3(60, 0, 0, 0);                  /* exit(0) */
    for (;;) ;
}
