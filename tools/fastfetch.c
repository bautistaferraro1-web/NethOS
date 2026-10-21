#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#define MAXL 24
#define MAXW 80

static void cpu_name(char *out)
{
    unsigned r[12], max;
    __asm__ volatile("cpuid" : "=a"(max) : "a"(0x80000000u) : "ebx", "ecx", "edx");
    if (max < 0x80000004u) { strcpy(out, "desconocido"); return; }
    for (int i = 0; i < 3; i++)
        __asm__ volatile("cpuid"
                         : "=a"(r[i*4]), "=b"(r[i*4+1]), "=c"(r[i*4+2]), "=d"(r[i*4+3])
                         : "a"(0x80000002u + i));
    memcpy(out, r, 48);
    out[48] = 0;
}

/* Lee el logo de /etc/nethos-logo.txt; devuelve la cantidad de lineas (0 si no existe) */
static int load_logo(char lines[MAXL][MAXW])
{
    FILE *f = fopen("/etc/nethra-logo.txt", "r");
    if (f == NULL) return 0;
    int n = 0;
    char buf[256];
    while (n < MAXL && fgets(buf, sizeof buf, f) != NULL) {
        size_t l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r' || buf[l-1] == ' ' || buf[l-1] == '\t')) l--;
        if (l > MAXW - 1) l = MAXW - 1;
        memcpy(lines[n], buf, l);
        lines[n][l] = 0;
        n++;
    }
    fclose(f);
    return n;
}

int main(void)
{
    static char logo[MAXL][MAXW];
    int nlogo = load_logo(logo);
    if (nlogo == 0) {
        static const char *def[] = {
            "  _    _   ______",
            " | \\ | | | _____|",
            " |  \\| | | |____",
            " | |\\  | | |____|",
            " | | \\ | | |____",
            " |_|  \\_ |______|",
        };
        for (int i = 0; i < 6; i++) strcpy(logo[nlogo++], def[i]);
    }
    int width = 0;
    for (int i = 0; i < nlogo; i++) { int l = (int)strlen(logo[i]); if (l > width) width = l; }
    width += 3;

    struct utsname u;
    uname(&u);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long up = ts.tv_sec, h = up / 3600, m = (up % 3600) / 60, s = up % 60;

    char cpu[64];
    cpu_name(cpu);
    const char *c = cpu;
    while (*c == ' ') c++;

    const char *sh = getenv("SHELL");
    char hostline[96], upline[64], kline[160], cpuline[80], shline[64], osline[32];
    snprintf(hostline, sizeof hostline, "%s@nethra", getenv("USER") ? getenv("USER") : "root");
    snprintf(osline, sizeof osline, "OS:     Nethra");
    snprintf(kline, sizeof kline, "Kernel: Nethel (%s %s)", u.sysname, u.machine);
    snprintf(upline, sizeof upline, "Uptime: %ldh %ldm %lds", h, m, s);
    snprintf(shline, sizeof shline, "Shell:  %s", sh ? sh : "bash");
    snprintf(cpuline, sizeof cpuline, "CPU:    %s", c);

    char memline[64];
    struct sysinfo si;
    if (sysinfo(&si) == 0 && si.totalram) {
        unsigned long tot = ((unsigned long)si.totalram * si.mem_unit) >> 20;
        unsigned long fre = ((unsigned long)si.freeram * si.mem_unit) >> 20;
        snprintf(memline, sizeof memline, "Memory: %lu MiB / %lu MiB", tot - fre, tot);
    } else snprintf(memline, sizeof memline, "Memory: desconocida");

    const char *info[] = { hostline, "-----------", osline, kline, upline, shline, cpuline, memline };
    int ninfo = 8;
    int rows = nlogo > ninfo ? nlogo : ninfo;
    for (int i = 0; i < rows; i++)
        printf("%-*s%s\n", width, i < nlogo ? logo[i] : "", i < ninfo ? info[i] : "");
    return 0;
}
