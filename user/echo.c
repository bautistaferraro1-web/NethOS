/* echo.elf: lee lineas del teclado con read() y las repite. 'salir' termina. */
#include "nlib.h"

static volatile int tope = 79;      /* .data (valor inicial distinto de cero) */
static volatile u64 lineas;         /* .bss */

void _start(void)
{
    char line[80];
    int len = 0;

    print("[echo.elf] escribi una linea y Enter. 'salir' para terminar.\n> ");

    for (;;) {
        char c;
        long n = sys_read(0, &c, 1);        /* bloquea hasta que haya una tecla */
        if (n < 0) sys_exit(1);
        if (n == 0) continue;

        if (c == '\n') {
            sys_write(1, &c, 1);
            line[len] = 0;
            if (streq(line, "salir")) break;
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
    sys_exit(0);
}
