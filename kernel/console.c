#include "console.h"

#define VGA ((volatile uint16_t *)0xB8000)
#define W 80
#define H 25

static int cx, cy;
static uint8_t color = 0x07;

void console_set_color(uint8_t fg, uint8_t bg) { color = (bg << 4) | (fg & 0x0F); }

static void scroll(void)
{
    for (int i = 0; i < W * (H - 1); i++) VGA[i] = VGA[i + W];
    for (int i = W * (H - 1); i < W * H; i++) VGA[i] = ((uint16_t)color << 8) | ' ';
    cy = H - 1;
}

void console_clear(void)
{
    for (int i = 0; i < W * H; i++) VGA[i] = ((uint16_t)color << 8) | ' ';
    cx = cy = 0;
}

void console_putc(char c)
{
    if (c == '\n') { cx = 0; cy++; }
    else if (c == '\r') { cx = 0; }
    else if (c == '\b') {
        if (cx > 0) { cx--; VGA[cy * W + cx] = ((uint16_t)color << 8) | ' '; }
    }
    else if (c == '\t') { cx = (cx + 8) & ~7; }
    else {
        VGA[cy * W + cx] = ((uint16_t)color << 8) | (uint8_t)c;
        cx++;
    }
    if (cx >= W) { cx = 0; cy++; }
    if (cy >= H) scroll();
}

void console_puts(const char *s) { while (*s) console_putc(*s++); }

void console_hex(uint64_t v)
{
    static const char d[] = "0123456789ABCDEF";
    console_puts("0x");
    for (int i = 60; i >= 0; i -= 4) console_putc(d[(v >> i) & 0xF]);
}

void console_dec(uint64_t v)
{
    char buf[21];
    int i = 20;
    buf[i] = 0;
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = '0' + (v % 10); v /= 10; }
    console_puts(&buf[i]);
}

void console_status(const char *s)
{
    const int region = 24;                   /* ancho fijo de la barra */
    for (int i = W - region; i < W; i++) VGA[i] = (0x1F << 8) | ' ';
    int len = 0;
    while (s[len]) len++;
    if (len > region) len = region;
    for (int i = 0; i < len; i++) VGA[W - len + i] = (0x1F << 8) | (uint8_t)s[i];
}
