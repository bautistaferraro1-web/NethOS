#include "console.h"
#include "cpu.h"
#include "mem.h"
#include "vmm.h"
#include "font8x16.h"

#define VGA ((volatile uint16_t *)P2V(0xB8000))
#define W 80
#define H 25
#define FB_VA 0xFFFFFF8000000000ULL
#define FB_PAGE_MASK (~(uint64_t)0xFFF)

static int cx, cy;
static uint8_t color = 0x07;

static int fb_active;
static uint32_t *fb_pixels;
static uint32_t fb_pitch, fb_width, fb_height;
static uint8_t frp, frs, fgp, fgs, fbp, fbs;
static int fb_cols, fb_rows;

static uint32_t fb_chan(uint8_t v, uint8_t size, uint8_t pos)
{
    if (!size || size > 8 || (unsigned)pos + size > 32) return 0;
    return ((((uint32_t)v * ((1U << size) - 1U) + 127U) / 255U) << pos);
}

static uint32_t fb_color(uint8_t r, uint8_t g, uint8_t b)
{
    return fb_chan(r, frs, frp) | fb_chan(g, fgs, fgp) | fb_chan(b, fbs, fbp);
}

static uint32_t fb_pal(uint8_t i)
{
    static const uint8_t p[16][3] = {
        {0,0,0},{0,0,170},{0,170,0},{0,170,170},
        {170,0,0},{170,0,170},{170,85,0},{170,170,170},
        {85,85,85},{85,85,255},{85,255,85},{85,255,255},
        {255,85,85},{255,85,255},{255,255,85},{255,255,255}
    };
    return fb_color(p[i & 15][0], p[i & 15][1], p[i & 15][2]);
}

static void fb_cell(int col, int row, uint8_t ch)
{
    if (col < 0 || row < 0 || col >= fb_cols || row >= fb_rows) return;
    uint32_t fg = fb_pal(color & 15), bg = fb_pal(color >> 4);
    const uint8_t *glyph = font8x16 + (unsigned)ch * 16U;
    for (int y = 0; y < 16; y++) {
        volatile uint32_t *d = (volatile uint32_t *)
            ((volatile uint8_t *)fb_pixels + (uint64_t)(row * 16 + y) * fb_pitch)
            + col * 8;
        for (int x = 0; x < 8; x++)
            d[x] = (glyph[y] & (0x80U >> x)) ? fg : bg;
    }
}

static void fb_fill(void)
{
    uint32_t bg = fb_pal(color >> 4);
    for (uint32_t y = 0; y < fb_height; y++) {
        volatile uint32_t *d = (volatile uint32_t *)
            ((volatile uint8_t *)fb_pixels + (uint64_t)y * fb_pitch);
        for (uint32_t x = 0; x < fb_width; x++) d[x] = bg;
    }
}

static void fb_scroll(void)
{
    if (fb_height <= 16) { fb_fill(); cy = 0; return; }
    for (uint32_t y = 16; y < fb_height; y++) {
        volatile uint32_t *d = (volatile uint32_t *)
            ((volatile uint8_t *)fb_pixels + (uint64_t)(y - 16) * fb_pitch);
        volatile uint32_t *s = (volatile uint32_t *)
            ((volatile uint8_t *)fb_pixels + (uint64_t)y * fb_pitch);
        for (uint32_t x = 0; x < fb_width; x++) d[x] = s[x];
    }
    uint32_t bg = fb_pal(color >> 4);
    for (uint32_t y = fb_height - 16; y < fb_height; y++) {
        volatile uint32_t *d = (volatile uint32_t *)
            ((volatile uint8_t *)fb_pixels + (uint64_t)y * fb_pitch);
        for (uint32_t x = 0; x < fb_width; x++) d[x] = bg;
    }
    cy = fb_rows - 1;
}

int console_init_framebuffer(uint64_t physical_info)
{
    if (!physical_info || physical_info >= 0x100000000ULL) return 0;
    const uint8_t *info = (const uint8_t *)P2V(physical_info);
    uint32_t total = *(const uint32_t *)info;
    if (total < 16 || total > 0x100000U) return 0;

    uint64_t off = 8, addr = 0, pitch = 0, width = 0, height = 0;
    uint8_t bpp = 0, type = 0, rpos = 0, rsize = 0;
    uint8_t gpos = 0, gsize = 0, bpos = 0, bsize = 0;
    int found = 0;

    while (off + 8 <= total) {
        const uint8_t *t = info + off;
        uint32_t id = *(const uint32_t *)t, size = *(const uint32_t *)(t + 4);
        if (size < 8 || off + size > total) return 0;
        if (id == 0) break;
        if (id == 8 && size >= 38) {
            addr = *(const uint64_t *)(t + 8);
            pitch = *(const uint32_t *)(t + 16);
            width = *(const uint32_t *)(t + 20);
            height = *(const uint32_t *)(t + 24);
            bpp = t[28]; type = t[29];
            if (type == 1 && bpp == 32) {
                rpos=t[32]; rsize=t[33]; gpos=t[34]; gsize=t[35];
                bpos=t[36]; bsize=t[37]; found=1; break;
            }
        }
        off = (off + size + 7) & ~7ULL;
    }

    if (!found || !addr || !width || !height || width > 8192 ||
        height > 8192 || pitch < width * 4 || pitch > 32768 ||
        !rsize || !gsize || !bsize ||
        (unsigned)rpos+rsize > 32 || (unsigned)gpos+gsize > 32 ||
        (unsigned)bpos+bsize > 32 || height > UINT64_MAX / pitch)
        return 0;

    uint64_t bytes = pitch * height;
    if (addr > UINT64_MAX - bytes) return 0;
    uint64_t base = addr & FB_PAGE_MASK, offset = addr - base;
    if (bytes > UINT64_MAX - offset) return 0;
    uint64_t span = offset + bytes;
    if (span > UINT64_MAX - 4095) return 0;
    uint64_t pages = (span + 4095) / 4096;
    if (!pages || pages > 0x40000000ULL / 4096) return 0;

    uint64_t done = 0;
    for (; done < pages; done++) {
        if (vmm_map(FB_VA + done * 4096, base + done * 4096,
                    VMM_WRITE | 0x008ULL | 0x010ULL) < 0) break;
    }
    if (done != pages) {
        while (done) vmm_unmap(FB_VA + --done * 4096);
        return 0;
    }

    fb_pitch=(uint32_t)pitch; fb_width=(uint32_t)width; fb_height=(uint32_t)height;
    frp=rpos; frs=rsize; fgp=gpos; fgs=gsize; fbp=bpos; fbs=bsize;
    fb_pixels=(uint32_t *)(FB_VA + offset);
    fb_cols=(int)(width/8); fb_rows=(int)(height/16);
    if (!fb_cols || !fb_rows) {
        while (done) vmm_unmap(FB_VA + --done * 4096);
        fb_pixels=0; return 0;
    }
    fb_active=1; cx=cy=0; fb_fill();
    return 1;
}

void console_set_color(uint8_t fg, uint8_t bg) { color = (bg << 4) | (fg & 0x0F); }

static void scroll(void)
{
    if (fb_active) { fb_scroll(); return; }
    for (int i = 0; i < W * (H - 1); i++) VGA[i] = VGA[i + W];
    for (int i = W * (H - 1); i < W * H; i++) VGA[i] = ((uint16_t)color << 8) | ' ';
    cy = H - 1;
}

void console_clear(void)
{
    if (fb_active) fb_fill();
    else for (int i = 0; i < W * H; i++) VGA[i] = ((uint16_t)color << 8) | ' ';
    cx = cy = 0;
}


/* Parser ANSI SGR: conserva su estado entre llamadas a console_putc(). */
static int ansi_state;
static int ansi_params[8];
static int ansi_count, ansi_value, ansi_have;
static int ansi_bold;

static uint8_t ansi_vga_color(int n)
{
    static const uint8_t map[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    if (n < 0 || n > 15) return 7;
    return n < 8 ? map[n] : (uint8_t)(map[n - 8] + 8);
}

static void ansi_sgr(void)
{
    if (ansi_have || ansi_count == 0) {
        if (ansi_count < 8)
            ansi_params[ansi_count++] = ansi_have ? ansi_value : 0;
    }

    for (int i = 0; i < ansi_count; i++) {
        int n = ansi_params[i];
        int fg = color & 15;
        int bg = (color >> 4) & 15;

        if (n == 0) {
            color = 0x07;
            ansi_bold = 0;
        } else if (n == 1) {
            ansi_bold = 1;
            if (fg < 8) fg += 8;
        } else if (n == 22) {
            ansi_bold = 0;
            if (fg >= 8) fg -= 8;
        } else if (n >= 30 && n <= 37) {
            fg = ansi_vga_color(n - 30 + (ansi_bold ? 8 : 0));
        } else if (n >= 90 && n <= 97) {
            fg = ansi_vga_color(n - 90 + 8);
        } else if (n >= 40 && n <= 47) {
            bg = ansi_vga_color(n - 40);
        } else if (n >= 100 && n <= 107) {
            bg = ansi_vga_color(n - 100 + 8);
        } else if (n == 39) {
            fg = 7;
        } else if (n == 49) {
            bg = 0;
        }

        color = (uint8_t)((bg << 4) | fg);
    }

    ansi_count = ansi_value = ansi_have = 0;
}

static void putc_raw(char c)
{
    if (ansi_state == 1) {
        ansi_state = (c == '[') ? 2 : 0;
        ansi_count = ansi_value = ansi_have = 0;
        return;
    }

    if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            if (ansi_value < 1000) ansi_value = ansi_value * 10 + c - '0';
            ansi_have = 1;
            return;
        }
        if (c == ';') {
            if (ansi_count < 8)
                ansi_params[ansi_count++] = ansi_have ? ansi_value : 0;
            ansi_value = ansi_have = 0;
            return;
        }
        if (c == 'm') ansi_sgr();
        ansi_state = 0;
        return;
    }

    if ((unsigned char)c == 27) {
        ansi_state = 1;
        return;
    }

    int cols = fb_active ? fb_cols : W;
    int rows = fb_active ? fb_rows : H;

    if (c == '\n') { cx = 0; cy++; }
    else if (c == '\r') cx = 0;
    else if (c == '\b') {
        if (cx > 0) {
            cx--;
            if (fb_active) fb_cell(cx, cy, ' ');
            else VGA[cy * W + cx] = ((uint16_t)color << 8) | ' ';
        }
    } else if (c == '\t') {
        cx = (cx + 8) & ~7;
    } else {
        if (fb_active) fb_cell(cx, cy, (uint8_t)c);
        else VGA[cy * W + cx] = ((uint16_t)color << 8) | (uint8_t)c;
        cx++;
    }

    if (cx >= cols) { cx = 0; cy++; }
    if (cy >= rows) scroll();
}

static void ser_putc(char ch);

void console_puts(const char *s)
{
    for (const char *q = s; *q; q++) ser_putc(*q);
    uint64_t f = irq_save();
    while (*s) putc_raw(*s++);
    irq_restore(f);
}

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
    const int region = 24;
    int len = 0;
    while (s[len]) len++;
    if (len > region) len = region;

    if (fb_active) {
        uint8_t old = color;
        color = 0x1F;
        int first = fb_cols - region;
        if (first < 0) first = 0;
        for (int i = first; i < fb_cols; i++) fb_cell(i, fb_rows - 1, ' ');
        int pos = fb_cols - len;
        if (pos < first) pos = first;
        for (int i = 0; i < len && pos + i < fb_cols; i++)
            fb_cell(pos + i, fb_rows - 1, (uint8_t)s[i]);
        color = old;
        return;
    }

    for (int i = W - region; i < W; i++) VGA[i] = (0x1F << 8) | ' ';
    for (int i = 0; i < len; i++) VGA[W - len + i] = (0x1F << 8) | (uint8_t)s[i];
}

/* ---- salida serie COM1 (para copiar la consola desde la terminal del host) ---- */
static inline void ser_outb(unsigned short p, unsigned char v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline unsigned char ser_inb(unsigned short p) { unsigned char v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void ser_putc(char ch)
{
    static int ready;
    if (!ready) {
        ser_outb(0x3F9, 0x00); ser_outb(0x3FB, 0x80); ser_outb(0x3F8, 0x01); ser_outb(0x3F9, 0x00);
        ser_outb(0x3FB, 0x03); ser_outb(0x3FA, 0xC7); ser_outb(0x3FC, 0x0B);
        ready = 1;
    }
    if (ch == '\n') { while (!(ser_inb(0x3FD) & 0x20)) { } ser_outb(0x3F8, '\r'); }
    while (!(ser_inb(0x3FD) & 0x20)) { }
    ser_outb(0x3F8, (unsigned char)ch);
}

void console_putc(char c)
{
    ser_putc((char)c);
    uint64_t f = irq_save();
    putc_raw(c);
    irq_restore(f);
}
