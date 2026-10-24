#define NLIB_MAIN
#include "nlib.h"
#include <stdint.h>

int nmain(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (argc > 1) return 7;                      /* modo hijo */
    char buf[256];
    long d;

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
    if (sys3(80, (long)"/etc", 0, 0) != 0) { print("[ftest] chdir /etc FALLO\n"); return 1; }
    n = sys3(79, (long)buf, sizeof buf, 0);
    if (n != 5) { print("[ftest] getcwd tras chdir FALLO\n"); return 1; }
    fd = sys3(2, (long)"hostname", 0, 0);
    if (fd < 0) { print("[ftest] open relativo FALLO\n"); return 1; }
    n = sys3(0, fd, (long)buf, 127);
    if (n <= 0) { print("[ftest] read hostname FALLO\n"); return 1; }
    buf[n] = 0; print("[ftest] hostname: "); print(buf);
    sys3(3, fd, 0, 0);
    fd = sys3(2, (long)"../hello.txt", 0, 0);
    if (fd < 0) { print("[ftest] open ../hello.txt FALLO\n"); return 1; }
    sys3(3, fd, 0, 0);
    if (sys3(80, (long)"..", 0, 0) != 0) { print("[ftest] chdir .. FALLO\n"); return 1; }
    n = sys3(79, (long)buf, sizeof buf, 0);
    if (n != 2) { print("[ftest] getcwd tras .. FALLO\n"); return 1; }
    d = sys3(2, (long)"/", 0, 0);
    if (d < 0 || sys3(0, d, (long)buf, 16) != -21) { print("[ftest] EISDIR FALLO\n"); return 1; }
    sys3(3, d, 0, 0);
    print("[ftest] ls /:\n");
    d = sys3(2, (long)"/", 0, 0);
    if (d < 0) { print("[ftest] open / FALLO\n"); return 1; }
    n = sys3(217, d, (long)buf, sizeof buf);
    if (n <= 0) { print("[ftest] getdents64 FALLO\n"); return 1; }
    for (long off = 0; off < n; ) {
        unsigned short rl = *(unsigned short *)(buf + off + 16);
        print("  "); print(buf + off + 19); print("\n");
        off += rl;
    }
    if (sys3(217, d, (long)buf, sizeof buf) != 0) { print("[ftest] getdents64 fin FALLO\n"); return 1; }
    sys3(3, d, 0, 0);
    print("[ftest] cwd OK\n");
    {
        long me = sys3(39, 0, 0, 0), pp = sys3(110, 0, 0, 0);
        if (me <= 0 || pp < 0) { print("[ftest] getpid FALLO\n"); return 1; }
        char *cv[3] = { "ftest", "child", 0 };
        long id = sys3(500, (long)"ftest", (long)cv, 0);
        if (id < 0) { print("[ftest] spawn hijo FALLO\n"); return 1; }
        int st = -1;
        long w = sys3(61, id, (long)&st, 0);
        if (w != id) { print("[ftest] wait4 pid FALLO\n"); return 1; }
        if (st != (7 << 8)) { print("[ftest] wait4 status FALLO\n"); return 1; }
        if (sys3(61, -1, (long)&st, 1) != -10) { print("[ftest] ECHILD FALLO\n"); return 1; }
        print("[ftest] wait4 OK\n");
    }
    {
        long pg = sys3(502, 0, 0, 0);
        if (pg <= 0) { print("[ftest] clone_space FALLO\n"); return 1; }
        print("[ftest] clone_space OK\n");
    }
    {
        volatile int x = 5;
        long pid = sys3(57, 0, 0, 0);
        if (pid < 0) { print("[ftest] fork FALLO\n"); return 1; }
        if (pid == 0) {
            x = 99;
            if (x != 99) return 1;
            print("[ftest hijo] hola desde el hijo\n");
            return 9;
        }
        int st = -1;
        if (sys3(61, pid, (long)&st, 0) != pid) { print("[ftest] fork wait4 FALLO\n"); return 1; }
        if (st != (9 << 8)) { print("[ftest] fork status FALLO\n"); return 1; }
        if (x != 5) { print("[ftest] fork: el hijo piso al padre\n"); return 1; }
        print("[ftest] fork OK\n");
    }

    {
        unsigned char st[144];
        long euid = sys3(107, 0, 0, 0);
        long egid = sys3(108, 0, 0, 0);

        print("[ftest] fchmod/fchown por descriptor...\n");

        long fd = sys3(2, (long)"/tmp/fdmeta-test",
                       0100 | 2 | 01000, 0666);
        if (fd < 0) {
            print("[ftest] crear archivo FALLO\n");
            return 1;
        }

        long rc = sys3(91, fd, 0640, 0);
        if (rc != 0) {
            print("[ftest] fchmod FALLO: ");
            print_num((u64)-rc);
            print("\n");
            sys3(3, fd, 0, 0);
            return 1;
        }

        rc = sys3(5, fd, (long)st, 0);
        if (rc != 0 ||
            ((*(unsigned int *)(st + 24)) & 07777u) != 0640u) {
            print("[ftest] fstat tras fchmod FALLO\n");
            sys3(3, fd, 0, 0);
            return 1;
        }
        print("[ftest] fchmod + fstat OK\n");

        if (euid == 0) {
            rc = sys3(93, fd, 1000, 1000);
            if (rc != 0) {
                print("[ftest] fchown FALLO: ");
                print_num((u64)-rc);
                print("\n");
                sys3(3, fd, 0, 0);
                return 1;
            }

            rc = sys3(5, fd, (long)st, 0);
            if (rc != 0 ||
                *(unsigned int *)(st + 28) != 1000u ||
                *(unsigned int *)(st + 32) != 1000u ||
                ((*(unsigned int *)(st + 24)) & 07777u) != 0640u) {
                print("[ftest] metadatos tras fchown FALLO\n");
                sys3(3, fd, 0, 0);
                return 1;
            }
            print("[ftest] fchown + propietario/grupo OK\n");
        } else {
            rc = sys3(93, fd, 0, 0);
            if (rc != -1) {
                print("[ftest] fchown sin privilegios: esperaba EPERM\n");
                sys3(3, fd, 0, 0);
                return 1;
            }

            rc = sys3(5, fd, (long)st, 0);
            if (rc != 0 ||
                *(unsigned int *)(st + 28) != (unsigned int)euid ||
                *(unsigned int *)(st + 32) != (unsigned int)egid) {
                print("[ftest] fchown modifico metadatos indebidamente\n");
                sys3(3, fd, 0, 0);
                return 1;
            }
            print("[ftest] fchown sin privilegios -> EPERM OK\n");
        }

        sys3(3, fd, 0, 0);
        print("[ftest] prueba de metadatos OK\n");
    }


    {
        unsigned char st[144];
        print("[ftest] permisos de ruta y directorios...\n");

        if (sys3(4, (long)"/tmp", (long)st, 0) != 0 ||
            ((*(uint32_t *)(st + 24)) & 07777u) != 01777u) {
            print("[ftest] /tmp no tiene modo 1777 FALLO\n");
            return 1;
        }

        long fd = sys3(2, (long)"/hello.txt/hijo", 0, 0);
        if (fd != -20) {
            print("[ftest] componente no directorio: esperaba ENOTDIR\n");
            if (fd >= 0) sys3(3, fd, 0, 0);
            return 1;
        }
        print("[ftest] componente no directorio -> ENOTDIR OK\n");

        fd = sys3(2, (long)"/tmp/ftest-parent-check",
                  0100 | 2 | 01000, 0666);
        if (fd < 0) {
            print("[ftest] crear en /tmp FALLO: ");
            print_num((u64)-fd);
            print("\n");
            return 1;
        }
        sys3(3, fd, 0, 0);
        if (sys3(87, (long)"/tmp/ftest-parent-check", 0, 0) != 0) {
            print("[ftest] borrar archivo propio en /tmp FALLO\n");
            return 1;
        }
        print("[ftest] crear/borrar en /tmp OK\n");

        long euid = sys3(107, 0, 0, 0);
        fd = sys3(2, (long)"/etc/ftest-write-check",
                  0100 | 2 | 01000, 0600);
        if (euid != 0) {
            if (fd != -13) {
                print("[ftest] crear en /etc sin privilegios: esperaba EACCES\n");
                if (fd >= 0) {
                    sys3(3, fd, 0, 0);
                }
                return 1;
            }
            print("[ftest] creación en /etc denegada -> EACCES OK\n");
        } else {
            if (fd < 0) {
                print("[ftest] root no pudo crear en /etc: ");
                print_num((u64)-fd);
                print("\n");
                return 1;
            }
            sys3(3, fd, 0, 0);
            if (sys3(87, (long)"/etc/ftest-write-check", 0, 0) != 0) {
                print("[ftest] limpiar archivo de prueba en /etc FALLO\n");
                return 1;
            }
            print("[ftest] creación como root en /etc OK\n");
        }
    }

    print("[ftest] todo OK\n");
    return 0;
}
