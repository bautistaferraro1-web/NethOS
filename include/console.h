#ifndef NETHEL_CONSOLE_H
#define NETHEL_CONSOLE_H
#include <stdint.h>

int console_init_framebuffer(uint64_t mb2_info);
void console_clear(void);
void console_set_color(uint8_t fg, uint8_t bg);
void console_putc(char c);
void console_puts(const char *s);
void console_hex(uint64_t v);
void console_dec(uint64_t v);
void console_status(const char *s);   /* texto fijo arriba a la derecha */

void console_get_size(uint16_t *rows, uint16_t *cols,
                      uint16_t *xpixel, uint16_t *ypixel);

#endif
