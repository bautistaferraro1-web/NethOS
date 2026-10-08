/* memt.elf: prueba brk, mmap, munmap, mprotect.
   memt          -> bateria completa
   memt ro       -> escribe en una pagina de solo lectura (tiene que morir con #PF)
   memt leak     -> reserva memoria y sale sin liberar (el kernel tiene que recuperarla) */
#define NLIB_MAIN
#include "nlib.h"

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("int $0x80" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "memory");
    return r;
}

static long brk_(u64 a)                         { return sys6(12, (long)a, 0, 0, 0, 0, 0); }
static long mmap_(u64 a, u64 l, int p, int f, int fd) { return sys6(9, (long)a, (long)l, p, f, fd, 0); }
static long munmap_(u64 a, u64 l)               { return sys6(11, (long)a, (long)l, 0, 0, 0, 0); }
static long mprotect_(u64 a, u64 l, int p)      { return sys6(10, (long)a, (long)l, p, 0, 0, 0); }
static long sys_spawn(const char *name, char **argv) { return sys3(500, (long)name, (long)argv, 0); }
static long sys_wait(long id)                   { return sys3(501, id, 0, 0); }

#define PROT_RW  3
#define MAP_PA   0x22          /* PRIVATE | ANONYMOUS */
#define MAP_FIXED 0x10

static int bad;
static void check(const char *what, int ok)
{
    print("  "); print(what); print(ok ? ": OK\n" : ": MAL\n");
    if (!ok) bad++;
}

int nmain(int argc, char **argv)
{
    if (argc > 1 && streq(argv[1], "ro")) {
        volatile char *p = (volatile char *)mmap_(0, 4096, PROT_RW, MAP_PA, -1);
        p[0] = 1;
        mprotect_((u64)p, 4096, 1);                     /* PROT_READ */
        print("[memt ro] escribiendo en pagina de solo lectura...\n");
        p[0] = 2;
        print("[memt ro] MAL: la escritura no fallo\n");
        return 1;
    }
    if (argc > 1 && streq(argv[1], "leak")) {
        for (int i = 0; i < 100; i++) {
            volatile char *p = (volatile char *)mmap_(0, 4 * 4096, PROT_RW, MAP_PA, -1);
            if ((long)p < 0) break;
            p[0] = 1;
        }
        brk_((u64)brk_(0) + 40 * 4096);
        return 0;                                       /* sin munmap: lo libera el kernel */
    }

    print("[memt] brk\n");
    u64 b0 = (u64)brk_(0);
    check("brk(0) devuelve el inicio, alineado", b0 != 0 && (b0 & 0xFFF) == 0);
    check("brk crece 10000 bytes", (u64)brk_(b0 + 10000) == b0 + 10000);
    volatile unsigned char *h = (volatile unsigned char *)b0;
    int ok = 1;
    for (u64 i = 0; i < 10000; i++) h[i] = (unsigned char)(i * 7);
    for (u64 i = 0; i < 10000; i++) if (h[i] != (unsigned char)(i * 7)) ok = 0;
    check("heap escribible y consistente", ok);
    check("brk encoge", (u64)brk_(b0 + 100) == b0 + 100);
    check("brk(0) = valor actual", (u64)brk_(0) == b0 + 100);
    check("brk bajo el inicio se ignora", (u64)brk_(b0 - 4096) == b0 + 100);
    brk_(b0 + 10000);
    check("paginas re-obtenidas vienen en cero", h[5000] == 0 && h[9000] == 0);
    check("brk absurdo falla sin cambiar nada", (u64)brk_(b0 + (1ULL << 40)) == b0 + 10000);

    print("[memt] mmap / munmap / mprotect\n");
    long m = mmap_(0, 3 * 4096, PROT_RW, MAP_PA, -1);
    check("mmap anonimo", m > 0 && (m & 0xFFF) == 0);
    volatile unsigned char *q = (volatile unsigned char *)m;
    q[0] = 11; q[4096] = 22; q[2 * 4096 + 4095] = 33;
    check("mmap escribible", q[0] == 11 && q[4096] == 22 && q[2 * 4096 + 4095] == 33);
    long m2 = mmap_(0, 4096, PROT_RW, MAP_PA, -1);
    check("segundo mmap no se solapa", m2 >= m + 3 * 4096);
    check("munmap", munmap_((u64)m, 3 * 4096) == 0);
    long m3 = mmap_((u64)m, 4096, PROT_RW, MAP_PA | MAP_FIXED, -1);
    check("MAP_FIXED en la misma direccion", m3 == m);
    check("pagina nueva viene en cero", q[0] == 0);
    check("mmap de archivo -> ENODEV", mmap_(0, 4096, PROT_RW, 0x02, 3) == -19);
    check("mmap len 0 -> EINVAL", mmap_(0, 0, PROT_RW, MAP_PA, -1) == -22);
    check("mprotect a solo lectura", mprotect_((u64)m, 4096, 1) == 0 && q[0] == 0);
    check("mprotect sin mapear -> ENOMEM", mprotect_(0x10000000ULL, 4096, 1) == -12);

    ok = 1;
    for (int i = 0; i < 200; i++) {
        long x = mmap_(0, 64 * 4096, PROT_RW, MAP_PA, -1);
        if (x < 0) { ok = 0; break; }
        ((volatile char *)x)[63 * 4096] = 1;
        munmap_((u64)x, 64 * 4096);
    }
    check("200 ciclos de mmap/munmap de 256 KiB", ok);

    print("[memt] hijo que escribe en solo lectura (tiene que morir con #PF):\n");
    char *cv[] = { "memt", "ro", 0 };
    long id = sys_spawn("memt", cv);
    if (id >= 0) sys_wait(id);

    print(bad ? "[memt] FALLO\n" : "[memt] todo OK\n");
    return bad ? 1 : 0;
}
