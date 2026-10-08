#define NLIB_MAIN
#include "nlib.h"

static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static int bad;
static void check(const char *what, int ok)
{
    print("[sctest] "); print(what); print(ok ? ": OK\n" : ": MAL\n");
    if (!ok) bad++;
}

int nmain(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char msg[] = "[sctest] hola via syscall\n";

    check("write por syscall", sc6(1, 1, (long)msg, (long)sizeof(msg) - 1, 0, 0, 0) == (long)sizeof(msg) - 1);

    u64 ok;
    __asm__ volatile(
        "mov $0x1111, %%rbx\n\t mov $0x2222, %%r12\n\t mov $0x3333, %%r13\n\t"
        "mov $0x4444, %%r14\n\t mov $0x5555, %%r15\n\t"
        "mov $24, %%eax\n\t syscall\n\t"                 /* sched_yield: fuerza un context switch */
        "xor %%edx, %%edx\n\t"
        "cmp $0x1111, %%rbx\n\t jne 1f\n\t"
        "cmp $0x2222, %%r12\n\t jne 1f\n\t"
        "cmp $0x3333, %%r13\n\t jne 1f\n\t"
        "cmp $0x4444, %%r14\n\t jne 1f\n\t"
        "cmp $0x5555, %%r15\n\t jne 1f\n\t"
        "mov $1, %%edx\n"
        "1:"
        : "=d"(ok) : : "rax", "rbx", "r12", "r13", "r14", "r15", "rcx", "r11", "memory");
    check("registros preservados tras yield", ok == 1);

    long m = sc6(9, 0, 4096, 3, 0x22, -1, 0);            /* mmap: usa r10, r8 */
    check("mmap con 6 args por syscall", m > 0);
    if (m > 0) { ((volatile char *)m)[0] = 7; check("pagina escribible", ((volatile char *)m)[0] == 7); }

    check("set_tid_address por syscall", sc6(218, (long)&bad, 0, 0, 0, 0, 0) > 0);
    check("syscall inexistente -> ENOSYS", sc6(999, 0, 0, 0, 0, 0, 0) == -38);
    check("int 0x80 sigue andando", sys_write(1, "", 0) == 0);

    print(bad ? "[sctest] FALLO\n" : "[sctest] todo OK\n");
    return bad ? 1 : 0;
}
