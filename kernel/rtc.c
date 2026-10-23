#include <stdint.h>
#include "rtc.h"
#include "cpu.h"
#include "io.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B
#define RTC_CENTURY  0x32

#define RTC_UIP      0x80
#define RTC_BINARY   0x04
#define RTC_24_HOUR  0x02
#define RTC_PM       0x80

struct rtc_snapshot {
    uint8_t second, minute, hour;
    uint8_t day, month, year, century;
    uint8_t status_b;
};

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static int wait_uip_clear(void)
{
    /* Espera acotada: nunca quedarse girando indefinidamente. */
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(cmos_read(RTC_STATUS_A) & RTC_UIP))
            return 0;
    }
    return -1;
}

static int read_snapshot(struct rtc_snapshot *s)
{
    if (wait_uip_clear() != 0)
        return -1;

    s->status_b = cmos_read(RTC_STATUS_B);
    s->second   = cmos_read(RTC_SECONDS);
    s->minute   = cmos_read(RTC_MINUTES);
    s->hour     = cmos_read(RTC_HOURS);
    s->day      = cmos_read(RTC_DAY);
    s->month    = cmos_read(RTC_MONTH);
    s->year     = cmos_read(RTC_YEAR);
    s->century  = cmos_read(RTC_CENTURY);

    /* Evita aceptar una muestra tomada durante una actualización. */
    if (cmos_read(RTC_STATUS_A) & RTC_UIP)
        return -1;

    return 0;
}

static int same_snapshot(const struct rtc_snapshot *a,
                         const struct rtc_snapshot *b)
{
    return a->second == b->second &&
           a->minute == b->minute &&
           a->hour == b->hour &&
           a->day == b->day &&
           a->month == b->month &&
           a->year == b->year &&
           a->century == b->century &&
           a->status_b == b->status_b;
}

static int decode_number(uint8_t raw, int binary, uint8_t *value)
{
    if (binary) {
        *value = raw;
        return 0;
    }

    uint8_t lo = raw & 0x0F;
    uint8_t hi = (raw >> 4) & 0x0F;

    if (lo > 9 || hi > 9)
        return -1;

    *value = (uint8_t)(hi * 10 + lo);
    return 0;
}

static int is_leap_year(int year)
{
    return (year % 4 == 0) &&
           ((year % 100 != 0) || (year % 400 == 0));
}

static int days_in_month(int year, int month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12)
        return 0;

    if (month == 2 && is_leap_year(year))
        return 29;

    return days[month - 1];
}

static int snapshot_to_epoch(const struct rtc_snapshot *s,
                             int64_t *result)
{
    int binary = !!(s->status_b & RTC_BINARY);
    int hour24 = !!(s->status_b & RTC_24_HOUR);
    int pm = !!(s->hour & RTC_PM);

    uint8_t second, minute, hour, day, month, year2;

    if (decode_number(s->second, binary, &second) ||
        decode_number(s->minute, binary, &minute) ||
        decode_number((uint8_t)(s->hour & 0x7F), binary, &hour) ||
        decode_number(s->day, binary, &day) ||
        decode_number(s->month, binary, &month) ||
        decode_number(s->year, binary, &year2))
        return -1;

    if (second > 59 || minute > 59)
        return -1;

    if (!hour24) {
        if (hour < 1 || hour > 12)
            return -1;

        hour %= 12;
        if (pm)
            hour += 12;
    }

    if (hour > 23)
        return -1;

    uint8_t century = 0;
    int century_ok =
        decode_number(s->century, binary, &century) == 0 &&
        century >= 19 && century <= 99;

    int year;
    if (century_ok) {
        year = (int)century * 100 + year2;
    } else {
        /* Convención de respaldo cuando el CMOS no ofrece siglo. */
        year = year2 >= 70 ? 1900 + year2 : 2000 + year2;
    }

    if (year < 1970 || year > 9999 ||
        month < 1 || month > 12 ||
        day < 1 || day > days_in_month(year, month))
        return -1;

    uint64_t days = 0;

    for (int y = 1970; y < year; y++)
        days += is_leap_year(y) ? 366 : 365;

    for (int m = 1; m < month; m++)
        days += (uint64_t)days_in_month(year, m);

    days += (uint64_t)(day - 1);

    uint64_t epoch = days * 86400ULL
                   + (uint64_t)hour * 3600ULL
                   + (uint64_t)minute * 60ULL
                   + second;

    *result = (int64_t)epoch;
    return 0;
}

int rtc_get_unix_seconds(int64_t *seconds)
{
    if (!seconds)
        return -1;

    /*
     * Protege la secuencia selector CMOS + lectura de datos contra
     * interrupciones y cambios de tarea durante el muestreo.
     */
    uint64_t flags = irq_save();
    int ok = 0;
    int64_t value = 0;

    for (unsigned attempt = 0; attempt < 4; attempt++) {
        struct rtc_snapshot a, b;

        if (read_snapshot(&a) != 0 ||
            read_snapshot(&b) != 0)
            continue;

        /* Dos muestras iguales evitan aceptar una fecha a medio actualizar. */
        if (!same_snapshot(&a, &b))
            continue;

        if (snapshot_to_epoch(&b, &value) == 0) {
            ok = 1;
            break;
        }
    }

    irq_restore(flags);

    if (!ok)
        return -1;

    *seconds = value;
    return 0;
}
