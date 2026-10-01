#include <stdint.h>
#include "keyboard.h"
#include "irq.h"
#include "io.h"

/* Scancode set 1, layout US, make codes 0x00-0x39 */
static const char normal[] =
    "\0\0" "1234567890-=" "\b" "\t" "qwertyuiop[]" "\n" "\0"
    "asdfghjkl;'`" "\0" "\\zxcvbnm,./" "\0" "*" "\0" " ";
static const char shifted[] =
    "\0\0" "!@#$%^&*()_+" "\b" "\t" "QWERTYUIOP{}" "\n" "\0"
    "ASDFGHJKL:\"~" "\0" "|ZXCVBNM<>?" "\0" "*" "\0" " ";

_Static_assert(sizeof(normal) - 1 == 58, "mapa normal incompleto");
_Static_assert(sizeof(shifted) - 1 == 58, "mapa shift incompleto");

#define BUF 256
static volatile uint8_t head, tail;
static char buf[BUF];
static int shift, ext;

static void push(char c)
{
    uint8_t n = (head + 1) % BUF;
    if (n == tail) return;                   /* buffer lleno: descartar */
    buf[head] = c;
    head = n;
}

static void kbd_irq(void)
{
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) { ext = 1; return; }     /* teclas extendidas: ignoradas por ahora */
    if (ext) { ext = 0; return; }

    uint8_t code = sc & 0x7F;
    int released = sc & 0x80;

    if (code == 0x2A || code == 0x36) { shift = !released; return; }
    if (released || code >= 58) return;

    char c = shift ? shifted[code] : normal[code];
    if (c) push(c);
}

void keyboard_init(void)
{
    irq_register(1, kbd_irq);
    pic_unmask(1);
}

int keyboard_getc(void)
{
    if (tail == head) return -1;
    char c = buf[tail];
    tail = (tail + 1) % BUF;
    return (unsigned char)c;
}
