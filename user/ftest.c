#define NLIB_MAIN
#include "nlib.h"
#include <stdint.h>

static int ftest_name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

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

    /* El directorio puede requerir varias lecturas de getdents64(). */
    for (;;) {
        n = sys3(217, d, (long)buf, sizeof buf);
        if (n < 0) {
            print("[ftest] getdents64 FALLO\n");
            sys3(3, d, 0, 0);
            return 1;
        }
        if (n == 0) break;

        for (long off = 0; off < n; ) {
            if (n - off < 19) {
                print("[ftest] dirent truncado FALLO\n");
                sys3(3, d, 0, 0);
                return 1;
            }

            unsigned short rl = *(unsigned short *)(buf + off + 16);
            if (rl < 20 || rl > n - off) {
                print("[ftest] reclen invalido FALLO\n");
                sys3(3, d, 0, 0);
                return 1;
            }

            print("  ");
            print(buf + off + 19);
            print("\n");
            off += rl;
        }
    }

    if (sys3(3, d, 0, 0) != 0) {
        print("[ftest] close directorio FALLO\n");
        return 1;
    }
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


    {
        char dbuf[256];
        unsigned char st[144];
        const char *dir = "/tmp/ftest-dir";
        const char *child = "/tmp/ftest-dir/child";

        print("[ftest] mkdir/rmdir y listado tmpfs...\n");

        long rc = sys3(83, (long)dir, 0755, 0);
        if (rc != 0) {
            print("[ftest] mkdir FALLO: ");
            print_num((u64)-rc);
            print("\n");
            return 1;
        }

        if (sys3(83, (long)dir, 0755, 0) != -17) {
            print("[ftest] mkdir repetido no devuelve EEXIST\n");
            return 1;
        }

        if (sys3(4, (long)dir, (long)st, 0) != 0 ||
            (*(uint32_t *)(st + 24) & 0170000u) != 0040000u) {
            print("[ftest] stat del directorio FALLO\n");
            return 1;
        }

        long fd = sys3(2, (long)child, 0100 | 2 | 01000, 0666);
        if (fd < 0) {
            print("[ftest] crear archivo hijo FALLO: ");
            print_num((u64)-fd);
            print("\n");
            return 1;
        }
        if (sys3(3, fd, 0, 0) != 0) {
            print("[ftest] close hijo FALLO\n");
            return 1;
        }

        long d = sys3(2, (long)dir, 0, 0);
        if (d < 0) {
            print("[ftest] open directorio nuevo FALLO\n");
            return 1;
        }

        int found_child = 0;
        for (;;) {
            long n = sys3(217, d, (long)dbuf, sizeof dbuf);
            if (n < 0) {
                print("[ftest] getdents directorio nuevo FALLO\n");
                sys3(3, d, 0, 0);
                return 1;
            }
            if (n == 0) break;

            for (long off = 0; off < n; ) {
                if (n - off < 19) {
                    print("[ftest] dirent truncado FALLO\n");
                    sys3(3, d, 0, 0);
                    return 1;
                }
                unsigned short rl = *(unsigned short *)(dbuf + off + 16);
                if (rl < 20 || rl > n - off) {
                    print("[ftest] reclen invalido FALLO\n");
                    sys3(3, d, 0, 0);
                    return 1;
                }
                if (ftest_name_eq(dbuf + off + 19, "child"))
                    found_child = 1;
                off += rl;
            }
        }
        sys3(3, d, 0, 0);

        if (!found_child) {
            print("[ftest] el hijo no aparece en getdents FALLO\n");
            return 1;
        }

        d = sys3(2, (long)"/tmp", 0, 0);
        if (d < 0) {
            print("[ftest] open /tmp FALLO\n");
            return 1;
        }

        int found_dir = 0;
        for (;;) {
            long n = sys3(217, d, (long)dbuf, sizeof dbuf);
            if (n < 0) {
                print("[ftest] getdents /tmp FALLO\n");
                sys3(3, d, 0, 0);
                return 1;
            }
            if (n == 0) break;

            for (long off = 0; off < n; ) {
                if (n - off < 19) {
                    print("[ftest] dirent /tmp truncado FALLO\n");
                    sys3(3, d, 0, 0);
                    return 1;
                }
                unsigned short rl = *(unsigned short *)(dbuf + off + 16);
                if (rl < 20 || rl > n - off) {
                    print("[ftest] reclen /tmp invalido FALLO\n");
                    sys3(3, d, 0, 0);
                    return 1;
                }
                if (ftest_name_eq(dbuf + off + 19, "ftest-dir"))
                    found_dir = 1;
                off += rl;
            }
        }
        sys3(3, d, 0, 0);

        if (!found_dir) {
            print("[ftest] /tmp no muestra el directorio creado FALLO\n");
            return 1;
        }

        if (sys3(84, (long)dir, 0, 0) != -39) {
            print("[ftest] rmdir no rechaza directorio no vacio\n");
            return 1;
        }

        if (sys3(87, (long)child, 0, 0) != 0) {
            print("[ftest] unlink del hijo FALLO\n");
            return 1;
        }
        if (sys3(84, (long)dir, 0, 0) != 0) {
            print("[ftest] rmdir directorio vacio FALLO\n");
            return 1;
        }

        if (sys3(84, (long)"/tmp/no-such-dir", 0, 0) != -2) {
            print("[ftest] rmdir inexistente no devuelve ENOENT\n");
            return 1;
        }

        long euid = sys3(107, 0, 0, 0);
        const char *etcdir = "/etc/ftest-mkdir-check";
        rc = sys3(83, (long)etcdir, 0755, 0);

        if (euid == 0) {
            if (rc != 0) {
                print("[ftest] root no puede crear directorio en /etc\n");
                return 1;
            }
            if (sys3(84, (long)etcdir, 0, 0) != 0) {
                print("[ftest] root no puede borrar directorio en /etc\n");
                return 1;
            }
            print("[ftest] mkdir/rmdir como root en /etc OK\n");
        } else {
            if (rc != -13) {
                print("[ftest] mkdir en /etc debio devolver EACCES\n");
                return 1;
            }
            print("[ftest] mkdir en /etc denegado -> EACCES OK\n");
        }

        print("[ftest] mkdir/rmdir y getdents OK\n");
    }


    {
        const char *root = "/tmp/ftest-nested-check";
        const char *sub = "/tmp/ftest-nested-check/sub";
        char cwd[128];

        print("[ftest] directorios anidados y rutas relativas...\n");

        /*
         * Limpieza preventiva por si una ejecución anterior se interrumpió.
         * Solo se usan nombres reservados para esta prueba.
         */
        sys3(87, (long)"/tmp/ftest-nested-check/sub/relative-check.txt", 0, 0);
        sys3(84, (long)sub, 0, 0);
        sys3(84, (long)root, 0, 0);

        long rc = sys3(83, (long)root, 0700, 0);
        if (rc != 0) {
            print("[ftest] mkdir directorio padre FALLO: ");
            print_num((u64)-rc);
            print("\n");
            return 1;
        }

        rc = sys3(83, (long)sub, 0700, 0);
        if (rc != 0) {
            print("[ftest] mkdir directorio hijo FALLO: ");
            print_num((u64)-rc);
            print("\n");
            return 1;
        }

        if (sys3(84, (long)root, 0, 0) != -39) {
            print("[ftest] rmdir padre con subdirectorio: esperaba ENOTEMPTY\n");
            return 1;
        }

        if (sys3(80, (long)sub, 0, 0) != 0) {
            print("[ftest] chdir directorio anidado FALLO\n");
            return 1;
        }

        long n = sys3(79, (long)cwd, sizeof cwd, 0);
        if (n <= 0 || !ftest_name_eq(cwd, sub)) {
            print("[ftest] getcwd en directorio anidado FALLO\n");
            return 1;
        }

        long fd = sys3(2, (long)"relative-check.txt",
                       0100 | 2 | 01000, 0600);
        if (fd < 0) {
            print("[ftest] crear archivo con ruta relativa FALLO: ");
            print_num((u64)-fd);
            print("\n");
            return 1;
        }

        if (sys3(3, fd, 0, 0) != 0) {
            print("[ftest] close archivo relativo FALLO\n");
            return 1;
        }

        if (sys3(84, (long)sub, 0, 0) != -16) {
            print("[ftest] rmdir directorio actual: esperaba EBUSY\n");
            return 1;
        }

        if (sys3(84, (long)root, 0, 0) != -39) {
            print("[ftest] rmdir padre no vacio: esperaba ENOTEMPTY\n");
            return 1;
        }

        if (sys3(87, (long)"relative-check.txt", 0, 0) != 0) {
            print("[ftest] unlink relativo FALLO\n");
            return 1;
        }

        if (sys3(80, (long)"..", 0, 0) != 0) {
            print("[ftest] chdir .. FALLO\n");
            return 1;
        }

        n = sys3(79, (long)cwd, sizeof cwd, 0);
        if (n <= 0 || !ftest_name_eq(cwd, root)) {
            print("[ftest] getcwd tras chdir .. FALLO\n");
            return 1;
        }

        if (sys3(84, (long)sub, 0, 0) != 0) {
            print("[ftest] rmdir subdirectorio vacio FALLO\n");
            return 1;
        }

        if (sys3(84, (long)root, 0, 0) != -16) {
            print("[ftest] rmdir directorio actual padre: esperaba EBUSY\n");
            return 1;
        }

        if (sys3(80, (long)"/", 0, 0) != 0) {
            print("[ftest] volver a / FALLO\n");
            return 1;
        }

        if (sys3(84, (long)root, 0, 0) != 0) {
            print("[ftest] rmdir directorio padre vacio FALLO\n");
            return 1;
        }

        print("[ftest] directorios anidados y rutas relativas OK\n");
    }


    {
        const char *dir = "/tmp/ftest-dir-perm-check";
        const char *child = "/tmp/ftest-dir-perm-check/probe";
        unsigned char st[144];

        print("[ftest] umask y permisos de busqueda...\n");

        /* Limpiar residuos de una ejecución anterior. */
        sys3(90, (long)dir, 0700, 0);
        sys3(87, (long)child, 0, 0);
        sys3(84, (long)dir, 0, 0);

        long oldmask = sys3(95, 0077, 0, 0);
        if (oldmask < 0) {
            print("[ftest] umask FALLO\n");
            return 1;
        }

        long rc = sys3(83, (long)dir, 0777, 0);
        sys3(95, oldmask, 0, 0);

        if (rc != 0) {
            print("[ftest] mkdir con umask FALLO: ");
            print_num((u64)-rc);
            print("\n");
            return 1;
        }

        if (sys3(4, (long)dir, (long)st, 0) != 0 ||
            (*(uint32_t *)(st + 24) & 0777u) != 0700u) {
            print("[ftest] mkdir no respeto umask 0077\n");
            return 1;
        }

        long fd = sys3(2, (long)child, 0100 | 2 | 01000, 0600);
        if (fd < 0) {
            print("[ftest] crear archivo antes de retirar X FALLO\n");
            sys3(84, (long)dir, 0, 0);
            return 1;
        }
        sys3(3, fd, 0, 0);

        if (sys3(90, (long)dir, 0600, 0) != 0) {
            print("[ftest] chmod directorio FALLO\n");
            sys3(87, (long)child, 0, 0);
            sys3(84, (long)dir, 0, 0);
            return 1;
        }

        fd = sys3(2, (long)child, 0, 0);
        long euid = sys3(107, 0, 0, 0);

        if (euid == 0) {
            if (fd < 0) {
                print("[ftest] root no puede atravesar directorio sin X\n");
                return 1;
            }
            sys3(3, fd, 0, 0);
        } else {
            if (fd != -13) {
                print("[ftest] abrir archivo sin X: esperaba EACCES\n");
                if (fd >= 0) sys3(3, fd, 0, 0);
                return 1;
            }
        }

        if (sys3(90, (long)dir, 0700, 0) != 0) {
            print("[ftest] restaurar permisos directorio FALLO\n");
            return 1;
        }

        if (sys3(87, (long)child, 0, 0) != 0 ||
            sys3(84, (long)dir, 0, 0) != 0) {
            print("[ftest] limpiar prueba de permisos FALLO\n");
            return 1;
        }

        if (sys3(84, (long)"/", 0, 0) != -16) {
            print("[ftest] rmdir / debería devolver EBUSY\n");
            return 1;
        }

        const char *regular = "/tmp/ftest-rmdir-file-check";
        long rfd = sys3(2, (long)regular, 0100 | 2 | 01000, 0600);

        if (rfd < 0) {
            print("[ftest] crear archivo para rmdir FALLO\n");
            return 1;
        }

        if (sys3(3, rfd, 0, 0) != 0) {
            sys3(87, (long)regular, 0, 0);
            print("[ftest] cerrar archivo para rmdir FALLO\n");
            return 1;
        }

        if (sys3(84, (long)regular, 0, 0) != -20) {
            sys3(87, (long)regular, 0, 0);
            print("[ftest] rmdir archivo debería devolver ENOTDIR\n");
            return 1;
        }

        if (sys3(87, (long)regular, 0, 0) != 0) {
            print("[ftest] limpiar archivo de prueba FALLO\n");
            return 1;
        }

        print("[ftest] umask, búsqueda y errores de rmdir OK\n");
    }


    {
        const char *file = "/disk/HELLO.TXT";

        print("[ftest] FAT32 de solo lectura...\n");

        /* Confirmar que el archivo conocido del volumen existe y se lee. */
        long fd = sys3(2, (long)file, 0, 0);
        if (fd < 0) {
            print("[ftest] abrir /disk/HELLO.TXT FALLO: ");
            print_num((u64)-fd);
            print("\n");
            return 1;
        }

        if (sys3(3, fd, 0, 0) != 0) {
            print("[ftest] cerrar archivo FAT32 FALLO\n");
            return 1;
        }

        long orc = sys3(2, (long)file, 1, 0); /* O_WRONLY */
        if (orc != -30) {
            print("[ftest] open O_WRONLY FAT32 deberia devolver EROFS: ");
            print_num((u64)-orc);
            print("\n");
            return 1;
        }

        orc = sys3(2, (long)file, 01000, 0); /* O_TRUNC */
        if (orc != -30) {
            print("[ftest] open O_TRUNC FAT32 deberia devolver EROFS: ");
            print_num((u64)-orc);
            print("\n");
            return 1;
        }

        orc = sys3(2, (long)"/disk/NETHRANEW.TXT", 0100 | 1, 0644);
        if (orc != -30) {
            print("[ftest] open O_CREAT FAT32 deberia devolver EROFS: ");
            print_num((u64)-orc);
            print("\n");
            return 1;
        }

        long rc = sys3(87, (long)file, 0, 0);
        if (rc != -30) {
            print("[ftest] unlink FAT32 debería devolver EROFS: ");
            print_num((u64)-rc);
            print("\n");
            return 1;
        }

        rc = sys3(83, (long)"/disk/NETHRATEST", 0755, 0);
        if (rc != -30) {
            print("[ftest] mkdir FAT32 debería devolver EROFS: ");
            print_num((u64)-rc);
            print("\n");
            return 1;
        }

        rc = sys3(90, (long)file, 0600, 0);
        if (rc != -30) {
            print("[ftest] chmod FAT32 debería devolver EROFS\n");
            return 1;
        }

        rc = sys3(92, (long)file, 1, 1);
        if (rc != -30) {
            print("[ftest] chown FAT32 debería devolver EROFS\n");
            return 1;
        }

        print("[ftest] FAT32 permanece de solo lectura OK\n");
    }


    {
        const char *overlay = "/etc/ftest-overlay-check";
        char dbuf[256];
        long euid = sys3(107, 0, 0, 0);

        int found_hostname = 0;
        int found_overlay = 0;
        long fd = -1;

        print("[ftest] integracion initramfs + tmpfs...\n");

        /* Copy-up de un archivo base: modificar el overlay y restaurar sus bytes. */
        if (euid == 0) {
            const char *path = "/hello.txt";
            char saved[256];
            char verify[256];
            const char changed[] = "COPYUP";
            long h = sys3(2, (long)path, 0, 0);
            if (h < 0) {
                print("[ftest] abrir archivo base para copy-up FALLO\n");
                return 1;
            }
            long saved_n = sys3(0, h, (long)saved, sizeof saved);
            long close_rc = sys3(3, h, 0, 0);
            if (close_rc != 0 || saved_n <= 0 || saved_n >= (long)sizeof saved) {
                print("[ftest] guardar contenido para copy-up FALLO\n");
                return 1;
            }

            h = sys3(2, (long)path, 1, 0); /* O_WRONLY: dispara copy-up */
            if (h < 0) {
                print("[ftest] abrir copia para escritura FALLO\n");
                return 1;
            }
            long wr = sys3(1, h, (long)changed, sizeof changed - 1);
            close_rc = sys3(3, h, 0, 0);
            if (wr != (long)(sizeof changed - 1) || close_rc != 0) {
                print("[ftest] escritura de copy-up FALLO\n");
                return 1;
            }

            h = sys3(2, (long)path, 0, 0);
            if (h < 0) {
                print("[ftest] reabrir copia superior FALLO\n");
                return 1;
            }
            long changed_n = sys3(0, h, (long)verify, sizeof verify);
            close_rc = sys3(3, h, 0, 0);
            if (close_rc != 0 || changed_n < (long)(sizeof changed - 1)) {
                print("[ftest] lectura de copia superior FALLO\n");
                return 1;
            }
            for (unsigned i = 0; i < sizeof changed - 1; i++) {
                if (verify[i] != changed[i]) {
                    print("[ftest] lectura no ve los datos de copy-up\n");
                    return 1;
                }
            }

            h = sys3(2, (long)path, 1 | 01000, 0600); /* O_WRONLY | O_TRUNC */
            if (h < 0) {
                print("[ftest] O_TRUNC sobre copia superior FALLO\n");
                return 1;
            }
            wr = sys3(1, h, (long)saved, (uint64_t)saved_n);
            close_rc = sys3(3, h, 0, 0);
            if (wr != saved_n || close_rc != 0) {
                print("[ftest] restaurar contenido tras O_TRUNC FALLO\n");
                return 1;
            }

            h = sys3(2, (long)path, 0, 0);
            if (h < 0) {
                print("[ftest] abrir archivo restaurado FALLO\n");
                return 1;
            }
            long restored_n = sys3(0, h, (long)verify, sizeof verify);
            close_rc = sys3(3, h, 0, 0);
            if (close_rc != 0 || restored_n != saved_n) {
                print("[ftest] tamaño restaurado incorrecto\n");
                return 1;
            }
            for (long i = 0; i < saved_n; i++) {
                if (verify[i] != saved[i]) {
                    print("[ftest] contenido restaurado incorrecto\n");
                    return 1;
                }
            }
            print("[ftest] copy-up y restauración del archivo base OK\n");
        }


        /*
         * Solo root crea la entrada: /etc no debe ser modificable
         * por el usuario normal. La entrada se elimina al terminar.
         */
        if (euid == 0) {
            fd = sys3(2, (long)overlay, 0100 | 2 | 01000, 0600);
            if (fd < 0) {
                print("[ftest] crear entrada overlay FALLO: ");
                print_num((u64)-fd);
                print("\n");
                return 1;
            }

            if (sys3(3, fd, 0, 0) != 0) {
                sys3(87, (long)overlay, 0, 0);
                print("[ftest] cerrar entrada overlay FALLO\n");
                return 1;
            }
        }


        {
            const char *base_file = "/etc/hostname";
            unsigned char base_st[144];

            print("[ftest] archivo base bajo overlay tmpfs...\n");

            /* El archivo original del initramfs debe seguir siendo legible. */
            long basefd = sys3(2, (long)base_file, 0, 0);
            if (basefd < 0) {
                print("[ftest] abrir hostname con overlay FALLO: ");
                print_num((u64)-basefd);
                print("\n");
                return 1;
            }

            long nr = sys3(0, basefd, (long)dbuf, sizeof dbuf);
            if (sys3(3, basefd, 0, 0) != 0) {
                print("[ftest] cerrar hostname con overlay FALLO\n");
                return 1;
            }

            if (nr < 6 ||
                dbuf[0] != 'n' || dbuf[1] != 'e' ||
                dbuf[2] != 't' || dbuf[3] != 'h' ||
                dbuf[4] != 'r' || dbuf[5] != 'a') {
                print("[ftest] contenido original de hostname alterado\n");
                return 1;
            }

            if (sys3(4, (long)base_file, (long)base_st, 0) != 0 ||
                (*(uint32_t *)(base_st + 24) & 0170000u) != 0100000u) {
                print("[ftest] stat de hostname bajo overlay FALLO\n");
                return 1;
            }

            /* O_CREAT|O_EXCL sobre un archivo existente debe dar EEXIST. */
            long op_rc = sys3(2, (long)base_file, 0100 | 0200 | 1, 0600);
            if (op_rc != -17) {
                print("[ftest] O_CREAT|O_EXCL sobre hostname debería dar EEXIST: ");
                print_num((u64)-op_rc);
                print("\n");
                return 1;
            }

            op_rc = sys3(2, (long)base_file, 0100, 0644);
            if (op_rc < 0) {
                print("[ftest] O_CREAT sobre archivo existente falló\n");
                return 1;
            }

            nr = sys3(0, op_rc, (long)dbuf, sizeof dbuf);
            {
                long cc = sys3(3, op_rc, 0, 0);
                if (cc == 0 && nr >= 6 &&
                    dbuf[0] == 'n' && dbuf[1] == 'e' &&
                    dbuf[2] == 't' && dbuf[3] == 'h' &&
                    dbuf[4] == 'r' && dbuf[5] == 'a') {
                } else {
                    print("[ftest] O_CREAT no conservó hostname\n");
                    return 1;
                }
            }

            op_rc = sys3(2, (long)"//etc//./hostname", 0, 0);
            if (op_rc < 0) {
                print("[ftest] ruta equivalente de hostname falló\n");
                return 1;
            }

            nr = sys3(0, op_rc, (long)dbuf, sizeof dbuf);
            {
                long cc = sys3(3, op_rc, 0, 0);
                if (cc == 0 && nr >= 6 &&
                    dbuf[0] == 'n' && dbuf[1] == 'e' &&
                    dbuf[2] == 't' && dbuf[3] == 'h' &&
                    dbuf[4] == 'r' && dbuf[5] == 'a') {
                } else {
                    print("[ftest] ruta equivalente alteró hostname\n");
                    return 1;
                }
            }

            print("[ftest] O_CREAT, escritura y rutas equivalentes OK\n");

            if (sys3(90, (long)base_file, 0600, 0) != -30) {
                print("[ftest] chmod del archivo base debería dar EROFS\n");
                return 1;
            }

            if (sys3(92, (long)base_file, 1, 1) != -30) {
                print("[ftest] chown del archivo base debería dar EROFS\n");
                return 1;
            }

            /*
             * unlink comprueba primero los permisos del directorio padre:
             * root recibe EROFS; el usuario sin escritura en /etc recibe EACCES.
             */
            long urc = sys3(87, (long)base_file, 0, 0);
            long uid = sys3(107, 0, 0, 0);
            long want = uid == 0 ? -30 : -13;

            if (urc != want) {
                print("[ftest] unlink del archivo base devolvió error inesperado: ");
                print_num((u64)-urc);
                print("\n");
                return 1;
            }

            print("[ftest] archivo base intacto y metadatos coherentes OK\n");
        }

        long d = sys3(2, (long)"/etc", 0, 0);
        if (d < 0) {
            if (euid == 0) sys3(87, (long)overlay, 0, 0);
            print("[ftest] abrir /etc FALLO\n");
            return 1;
        }

        for (;;) {
            long n = sys3(217, d, (long)dbuf, sizeof dbuf);

            if (n < 0) {
                sys3(3, d, 0, 0);
                if (euid == 0) sys3(87, (long)overlay, 0, 0);
                print("[ftest] getdents /etc FALLO\n");
                return 1;
            }

            if (n == 0) break;

            for (long off = 0; off < n; ) {
                if (n - off < 19) {
                    sys3(3, d, 0, 0);
                    if (euid == 0) sys3(87, (long)overlay, 0, 0);
                    print("[ftest] dirent /etc truncado FALLO\n");
                    return 1;
                }

                unsigned short rl = *(unsigned short *)(dbuf + off + 16);
                if (rl < 20 || rl > n - off) {
                    sys3(3, d, 0, 0);
                    if (euid == 0) sys3(87, (long)overlay, 0, 0);
                    print("[ftest] reclen /etc invalido FALLO\n");
                    return 1;
                }

                const char *name = dbuf + off + 19;

                if (ftest_name_eq(name, "hostname"))
                    found_hostname = 1;

                if (ftest_name_eq(name, "ftest-overlay-check"))
                    found_overlay = 1;

                off += rl;
            }
        }

        if (sys3(3, d, 0, 0) != 0) {
            if (euid == 0) sys3(87, (long)overlay, 0, 0);
            print("[ftest] cerrar /etc FALLO\n");
            return 1;
        }

        if (!found_hostname) {
            if (euid == 0) sys3(87, (long)overlay, 0, 0);
            print("[ftest] el overlay oculto perdio hostname del initramfs\n");
            return 1;
        }

        if (euid == 0 && !found_overlay) {
            sys3(87, (long)overlay, 0, 0);
            print("[ftest] getdents /etc no muestra el archivo tmpfs\n");
            return 1;
        }

        if (euid == 0) {
            if (sys3(87, (long)overlay, 0, 0) != 0) {
                print("[ftest] limpiar archivo overlay FALLO\n");
                return 1;
            }
            print("[ftest] entradas initramfs + tmpfs visibles OK\n");
        } else {
            print("[ftest] entradas originales del initramfs visibles OK\n");
        }
    }

    print("[ftest] todo OK\n");
    return 0;
}
