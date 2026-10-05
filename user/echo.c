/* echo.elf: lee lineas del teclado con read() y las repite. 'salir' termina. */
#define NLIB_MAIN
#include "nlib.h"

static volatile int tope = 79;      /* .data (valor inicial distinto de cero) */
static volatile u64 lineas;         /* .bss */

int nmain(int argc, char **argv)
{
    char line[80];
    int len = 0;

    if (argc > 1) {                     /* modo comando: echo hola mundo */
        for (int i = 1; i < argc; i++) { if (i > 1) print(" "); print(argv[i]); }
        print("\n");
        return 0;
    }

    print("[echo.elf] escribi una linea y Enter. 'salir' o 'exit' para terminar.\n> ");

    for (;;) {
        char c;
        long n = sys_read(0, &c, 1);        /* bloquea hasta que haya una tecla */
        if (n < 0) sys_exit(1);
        if (n == 0) continue;

        if (c == '\n') {
            sys_write(1, &c, 1);
            line[len] = 0;
            if (streq(line, "salir") || streq(line, "exit")) break;
            lineas++;
            print("tu escribiste ("); print_num((u64)len); print(" chars): ");
            print(line); print("\n> ");
            len = 0;
        } else if (c == '\b') {
            if (len > 0) { len--; sys_write(1, &c, 1); }
        } else if (c >= 32 && len < tope) {
            line[len++] = c;
            sys_write(1, &c, 1);
        }
    }

    print("[echo.elf] lineas: "); print_num(lineas); print("\n");
    return 0;
}
