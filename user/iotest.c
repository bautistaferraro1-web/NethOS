#define NLIB_MAIN
#include "nlib.h"

static char big[2048];

int nmain(int argc, char **argv)
{
    (void)argc; (void)argv;
    int bad = 0;

    static const char a[] = "[", b[] = "writev", c[] = "] OK\n";
    struct nethel_iovec iov[3] = { { a, slen(a) }, { b, slen(b) }, { c, slen(c) } };
    long r = sys_writev(1, iov, 3);
    if (r != 12) { print("[iotest] writev MAL\n"); bad++; }

    /* TCGETS escribe 36 bytes (struct termios del kernel); el resto no se toca */
    unsigned char t[64];
    for (int i = 0; i < 64; i++) t[i] = 0xAA;
    r = sys_ioctl(0, 0x5401, t);
    int ok = (r == 0);
    for (int i = 0; i < 36; i++) if (t[i] != 0) ok = 0;
    for (int i = 36; i < 64; i++) if (t[i] != 0xAA) ok = 0;
    print(ok ? "[iotest] ioctl TCGETS OK\n" : "[iotest] ioctl TCGETS MAL\n");
    if (!ok) bad++;

    for (int i = 0; i < 1999; i++) big[i] = 'A';
    big[1999] = '\n';
    r = sys_write(1, big, 2000);
    print(r == 2000 ? "[iotest] write de 2000 bytes OK\n" : "[iotest] write grande MAL\n");
    if (r != 2000) bad++;

    r = sys_writev(1, (const struct nethel_iovec *)0x10, 1);
    print(r == -14 ? "[iotest] puntero malo -> EFAULT OK\n" : "[iotest] EFAULT MAL\n");
    if (r != -14) bad++;

    print(bad ? "[iotest] FALLO\n" : "[iotest] todo OK\n");
    return bad ? 1 : 0;
}
