#define NLIB_MAIN
#include "nlib.h"

int nmain(int argc, char **argv)
{
    (void)argc; (void)argv;
    int bad = 0;

    /* 1) archivo inexistente: error y el proceso sigue vivo */
    char *a0[] = { "/nope", 0 };
    long r = sys3(59, (long)"/nope", (long)a0, 0);
    print(r == -2 ? "[exectest] ENOENT OK\n" : "[exectest] ENOENT MAL\n");
    if (r != -2) bad++;

    /* 2) un directorio no es ejecutable: EACCES (-13) */
    r = sys3(59, (long)"/bin", (long)a0, 0);
    print(r == -13 ? "[exectest] EACCES OK\n" : "[exectest] EACCES MAL\n");
    if (r != -13) bad++;

    /* 3) fork + execve + wait4 */
    long pid = sys3(57, 0, 0, 0);
    if (pid == 0) {
        char *av[] = { "/bin/hello", 0 };
        sys3(59, (long)"/bin/hello", (long)av, 0);
        print("[exectest hijo] execve volvio, MAL\n");
        sys3(60, 99, 0, 0);
    }
    int st = -1;
    long w = sys3(61, pid, (long)&st, 0);
    int ok = (pid > 0 && w == pid && st == 0);
    print(ok ? "[exectest] fork+exec+wait4 OK\n" : "[exectest] fork+exec+wait4 MAL\n");
    if (!ok) bad++;

    print(bad ? "[exectest] FALLO\n" : "[exectest] todo OK\n");
    return bad ? 1 : 0;
}
