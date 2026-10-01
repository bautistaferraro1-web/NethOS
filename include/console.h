#ifndef NETHEL_CONSOLE_H
#define NETHEL_CONSOLE_H
#include <stdint.h>

void console_clear(void);
void console_set_color(uint8_t fg, uint8_t bg);
void console_putc(char c);
void console_puts(const char *s);
void console_hex(uint64_t v);
void console_dec(uint64_t v);
void console_status(const char *s);   /* texto fijo arriba a la derecha */

#endif
