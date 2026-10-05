/* abi.elf: prueba arch_prctl (FS por proceso), set_tid_address y auxv.
   El padre vuelca el auxv y lanza un hijo; cada uno usa un FS distinto y gira
   (preemptado). Si el kernel no guarda FS_BASE por tarea, fs:0 lee cualquier cosa. */
#define NLIB_MAIN
#include "nlib.h"

static long sys_spawn(const char *name, char **argv) { return sys3(500, (long)name, (long)argv, 0); }
static long sys_wait(long id)                        { return sys3(501, id, 0, 0); }
static long arch_prctl(long c, long a)               { return sys3(158, c, a, 0); }
static long set_tid(void *p)                         { return sys3(218, (long)p, 0, 0); }

static u64 tls_area[2][8];      /* una direccion distinta para padre e hijo */
static u64 tid_slot;

static u64 fs0(void) { u64 v; __asm__ volatile("movq %%fs:0, %0" : "=r"(v)); return v; }

int nmain(int argc, char **argv)
{
    int child = argc > 1;
    const char *tag = child ? "[abi hijo]  " : "[abi padre] ";
    u64 me = child ? 0xC1C1C1C1ULL : 0xA0A0A0A0ULL;
    long id = 0;
    int bad = 0;

    if (!child) {
        char **e = argv + argc + 1;          /* saltar argv y envp */
        while (*e) e++;
        u64 *av = (u64 *)(e + 1);
        print("[abi] auxv (tipo = valor):\n");
        for (; av[0]; av += 2) { print("  "); print_num(av[0]); print(" = "); print_num(av[1]); print("\n"); }

        char *cv[] = { "abi", "hijo", 0 };
        id = sys_spawn("abi", cv);
        if (id < 0) { print("abi: no pude lanzar al hijo\n"); return 1; }
    }

    long tid = set_tid(&tid_slot);
    print(tag); print("set_tid_address -> tid "); print_num((u64)tid);
    if (tid > 0) print(" OK\n"); else { print(" MAL\n"); bad++; }

    tls_area[child][0] = me;
    long rc = arch_prctl(0x1002, (long)tls_area[child]);       /* ARCH_SET_FS */
    u64 got = 0;
    long rc2 = arch_prctl(0x1003, (long)&got);                 /* ARCH_GET_FS */
    int ok = rc == 0 && rc2 == 0 && got == (u64)tls_area[child];
    print(tag); print(ok ? "arch_prctl SET/GET_FS OK\n" : "arch_prctl SET/GET_FS MAL\n");
    if (!ok) bad++;

    long bogus = arch_prctl(0x9999, 0);
    print(tag); print(bogus == -22 ? "codigo invalido -> EINVAL OK\n" : "codigo invalido MAL\n");
    if (bogus != -22) bad++;

    for (int r = 1; r <= 4; r++) {
        for (volatile u64 i = 0; i < 30000000ULL; i++) ;
        int good = fs0() == me;
        print(tag); print("ronda "); print_num((u64)r);
        print(good ? " fs:0 OK\n" : " fs:0 CORRUPTO\n");
        if (!good) bad++;
    }

    if (!child) sys_wait(id);
    print(tag); print(bad ? "FALLO\n" : "todo OK\n");
    return bad ? 1 : 0;
}
