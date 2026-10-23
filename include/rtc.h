#ifndef NETHEL_RTC_H
#define NETHEL_RTC_H

#include <stdint.h>

/* Devuelve 0 y escribe segundos Unix si el RTC es válido; -1 si falla. */
int rtc_get_unix_seconds(int64_t *seconds);

#endif
