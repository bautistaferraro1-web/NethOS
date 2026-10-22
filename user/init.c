#define NLIB_MAIN
#include "nlib.h"

static void cat2(char *d, const char *a, const char *b)
{
    while (*a) *d++ = *a++;
    while (*b) *d++ = *b++;
    *d = 0;
}

static void yield_(void) { sys3(24, 0, 0, 0); }

static void show_file(const char *path)
{
    static char b[1024];
    long fd = sys3(2, (long)path, 0, 0);
    if (fd < 0) return;
    long n;
    while ((n = sys_read((int)fd, b, sizeof b)) > 0) sys_write(1, b, (u64)n);
    sys3(3, fd, 0, 0);
}

static int ask(const char *prompt, char *buf, int max)
{
    for (;;) {
        print(prompt);
        long r = sys_read(0, buf, (u64)(max - 1));
        if (r == -4) continue;
        if (r <= 0) { print("\n"); yield_(); continue; }
        if (buf[r - 1] == '\n') r--;
        buf[r] = 0;
        return (int)r;
    }
}

static int lookup(const char *user, char *home, char *shell)
{
    static char data[1024];
    long fd = sys3(2, (long)"/etc/passwd", 0, 0);
    if (fd < 0) return 0;
    long n = sys_read((int)fd, data, sizeof data - 1);
    sys3(3, fd, 0, 0);
    if (n <= 0) return 0;
    data[n] = 0;
    char *p = data;
    while (*p) {
        char *f[7];
        int k = 0;
        f[k++] = p;
        char *q = p;
        while (*q && *q != '\n') {
            if (*q == ':' && k < 7) { *q = 0; f[k++] = q + 1; }
            q++;
        }
        int more = (*q == '\n');
        *q = 0;
        if (k == 7 && streq(f[0], user)) {
            int i = 0;
            while (f[5][i] && i < 63) { home[i] = f[5][i]; i++; }
            home[i] = 0;
            i = 0;
            while (f[6][i] && i < 63) { shell[i] = f[6][i]; i++; }
            shell[i] = 0;
            return 1;
        }
        p = more ? q + 1 : q;
    }
    return 0;
}

static void login(void)
{
    static char user[64], home[64], shell[64];
    static char e_home[80], e_user[80], e_shell[80];
    /* Crear una sesión propia antes de entregar la consola interactiva a Bash. */
    long sid = sys3(112, 0, 0, 0);            /* setsid */
    if (sid < 0) {
        print("login: setsid fallo\n");
        return;
    }

    sys3(16, 0, 0x5F01, 1);                 /* consola cocinada, y esta tarea pasa a ser la duena */
    show_file("/etc/issue");
    for (;;) {
        if (ask("nethra login: ", user, 64) == 0) continue;
        if (lookup(user, home, shell)) break;
        print("Usuario desconocido\n");
    }
    cat2(e_home, "HOME=", home);
    cat2(e_user, "USER=", user);
    cat2(e_shell, "SHELL=", shell);
    char *envp[] = { e_home, e_user, e_shell, "PATH=/bin", "TERM=nethra", "PS1=\\u@nethra:\\w\\$ ", 0 };
    char *av[] = { "bash", "--noprofile", "--norc", 0 };
    sys3(59, (long)shell, (long)av, (long)envp);
    print("login: no pude ejecutar el shell\n");
}

int nmain(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (;;) {
        long pid = sys3(57, 0, 0, 0);       /* fork */
        if (pid == 0) { login(); sys_exit(1); }
        if (pid < 0) { print("init: fork fallo\n"); for (int i = 0; i < 100; i++) yield_(); continue; }
        int st = 0;
        for (;;) {
            long r = sys3(61, pid, (long)&st, 0);   /* wait4 */
            if (r == pid) break;
            if (r < 0 && r != -4) break;
            yield_();
        }
    }
}
