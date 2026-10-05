/* loop.elf: se cuelga en Ring 3 sin hacer syscalls. Sirve para probar Ctrl+C. */
#include "nlib.h"

static volatile u64 vueltas;

void _start(void)
{
    print("[loop.elf] girando sin syscalls. Ctrl+C para matarme.\n");
    for (;;) vueltas++;
}
