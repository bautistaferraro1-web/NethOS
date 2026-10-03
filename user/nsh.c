/* nsh: shell de Nethel. Lee una linea, lanza el programa con spawn y espera con wait. */
#include "nlib.h"

static volatile int maxlen = 79;    /* .data */
static volatile u64 comandos;       /* .bss  */

static long sys_spawn(const char *name) { return sys3(500, (long)name, 0, 0); }
static long sys_wait(long id)           { return sys3(501, id, 0, 0); }

static void run(const char *line)
{
    if (!line[0]) return;
    if (streq(line, "exit")) { print("nsh: chau\n"); sys_exit(0); }
    if (streq(line, "help")) {
        print("nsh: internos: help exit | programas: hello echo nsh\n");
        return;
    }
    comandos++;
    long id = sys_spawn(line);
    if (id < 0) { print("nsh: no encontrado: "); print(line); print("\n"); return; }
    sys_wait(id);
}

void _start(void)
{
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
