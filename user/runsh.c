#define NLIB_MAIN
#include "nlib.h"

int nmain(int argc, char **argv)
{
    (void)argc; (void)argv;
    char *av[] = { "bash", "--noprofile", "--norc", 0 };
    sys3(16, 0, 0x5F01, 1);   /* consola cocinada: eco + linea */
    print("[runsh] execve /bin/bash\n");
    long r = sys3(59, (long)"/bin/bash", (long)av, 0);
    print("[runsh] execve fallo\n");
    return (int)(-r);
}
