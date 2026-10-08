/* nsh: shell de Nethel. Lee una linea, la parte en palabras, lanza el programa
   con spawn(nombre, argv) y espera con wait. */
#define NLIB_MAIN
#include "nlib.h"

#define MAXARGS 8

static volatile int maxlen = 79;    /* .data */
static volatile u64 comandos;       /* .bss  */

static long sys_spawn(const char *name, char **argv) { return sys3(500, (long)name, (long)argv, 0); }
static long sys_wait(long id)                        { return sys3(501, id, 0, 0); }

static void run(char *line)
{
    char *av[MAXARGS + 1];
    int ac = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        if (ac == MAXARGS) { print("nsh: demasiados argumentos\n"); return; }
        av[ac++] = p;
        while (*p && *p != ' ') p++;
    }
    av[ac] = 0;
    if (!ac) return;

    if (streq(av[0], "exit")) { print("nsh: chau\n"); sys_exit(0); }
    if (streq(av[0], "help")) {
        print("nsh: internos: help exit | programas: hello echo nsh loop sse abi memt iotest sctest\n");
        return;
    }
    comandos++;
    long id = sys_spawn(av[0], av);
    if (id == -7) { print("nsh: argumentos muy largos\n"); return; }
    if (id < 0)   { print("nsh: no encontrado: "); print(av[0]); print("\n"); return; }
    sys_wait(id);
}

int nmain(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[80];
    int len = 0;

    print("[nsh] shell de Nethel. 'help' para ayuda.\n$ ");
    for (;;) {
        char c;
        long n = sys_read(0, &c, 1);
        if (n < 0) sys_exit(1);
        if (n == 0) continue;

        if (c == '\n') {
            sys_write(1, &c, 1);
            line[len] = 0;
            len = 0;
            run(line);
            print("$ ");
        } else if (c == '\b') {
            if (len > 0) { len--; sys_write(1, &c, 1); }
        } else if (c >= 32 && len < maxlen) {
            line[len++] = c;
            sys_write(1, &c, 1);
        }
    }
}
