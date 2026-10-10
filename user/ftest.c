#include "nlib.h"

int nmain(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[128];

    print("[ftest] open /hello.txt... ");
    long fd = sys3(2, (long)"/hello.txt", 0, 0);
    if (fd < 0) { print("FALLO\n"); return 1; }
    print("fd OK\n");

    long n = sys3(0, fd, (long)buf, 127);
    if (n <= 0) { print("[ftest] read FALLO\n"); return 1; }
    buf[n] = 0;
    print(buf);

    if (sys3(8, fd, 0, 0) != 0) { print("[ftest] lseek FALLO\n"); return 1; }
    if (sys3(0, fd, (long)buf, 4) != 4) { print("[ftest] re-read FALLO\n"); return 1; }

    if (sys3(3, fd, 0, 0) != 0) { print("[ftest] close FALLO\n"); return 1; }
    if (sys3(2, (long)"/no-existe", 0, 0) != -2) { print("[ftest] ENOENT FALLO\n"); return 1; }

    n = sys3(79, (long)buf, sizeof buf, 0);
    if (n != 2 || buf[0] != '/' || buf[1]) { print("[ftest] getcwd FALLO\n"); return 1; }
    if (sys3(79, (long)buf, 1, 0) != -34) { print("[ftest] getcwd ERANGE FALLO\n"); return 1; }
    if (sys3(80, (long)"/", 0, 0) != 0) { print("[ftest] chdir / FALLO\n"); return 1; }
    if (sys3(80, (long)"/hello.txt", 0, 0) != -20) { print("[ftest] chdir archivo FALLO\n"); return 1; }
    if (sys3(80, (long)"/nada", 0, 0) != -2) { print("[ftest] chdir inexistente FALLO\n"); return 1; }
    fd = sys3(2, (long)"./hello.txt", 0, 0);
    if (fd < 0) { print("[ftest] open ./hello.txt FALLO\n"); return 1; }
    sys3(3, fd, 0, 0);
    print("[ftest] cwd OK\n");
    print("[ftest] todo OK\n");
    return 0;
}
