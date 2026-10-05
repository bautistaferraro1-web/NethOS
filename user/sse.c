/* sse.elf: prueba que el estado SSE se guarda por proceso.
   El padre lanza un hijo; cada uno guarda un patron distinto en xmm0 y gira
   (preemptado por el timer). Si el kernel no guarda/restaura XMM, se corrompe. */
#define NLIB_MAIN
#include "nlib.h"

static long sys_spawn(const char *name, char **argv) { return sys3(500, (long)name, (long)argv, 0); }
static long sys_wait(long id)                        { return sys3(501, id, 0, 0); }

/* Todo en un solo asm: el compilador no puede tocar xmm0 mientras giramos */
static int check(u64 pat, u64 iters)
{
    u64 lo, hi;
    __asm__ volatile(
        "movq %[p], %%xmm0\n\t"
        "punpcklqdq %%xmm0, %%xmm0\n\t"
        "1: dec %[n]\n\t"
        "jnz 1b\n\t"
        "movq %%xmm0, %[lo]\n\t"
        "movhlps %%xmm0, %%xmm1\n\t"
        "movq %%xmm1, %[hi]\n\t"
        : [lo] "=&r"(lo), [hi] "=&r"(hi), [n] "+r"(iters)
        : [p] "r"(pat)
        : "xmm0", "xmm1", "cc");
    return lo == pat && hi == pat;
}

int nmain(int argc, char **argv)
{
    (void)argv;
    int child = argc > 1;
    u64 pat = child ? 0xBBBBBBBB22222222ULL : 0xAAAAAAAA11111111ULL;
    const char *tag = child ? "[sse hijo]  " : "[sse padre] ";
    long id = 0;

    if (!child) {
        char *av[] = { "sse", "hijo", 0 };
        id = sys_spawn("sse", av);
        if (id < 0) { print("sse: no pude lanzar al hijo\n"); return 1; }
    }

    volatile double a = 1.5, b = 2.25;        /* SSE generado por gcc */
    long fl = (long)(a * b * 1000.0);
    print(tag); print("float 1.5*2.25*1000 = "); print_num((u64)fl);
    print(fl == 3375 ? " OK\n" : " MAL\n");

    int bad = 0;
    for (int r = 1; r <= 4; r++) {
        int ok = check(pat, 50000000ULL);
        print(tag); print("ronda "); print_num((u64)r);
        print(ok ? " xmm0 OK\n" : " xmm0 CORRUPTO\n");
        if (!ok) bad++;
    }

    if (!child) sys_wait(id);
    print(tag); print(bad ? "FALLO\n" : "todo OK\n");
    return bad ? 1 : 0;
}
