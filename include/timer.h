#ifndef NETHEL_TIMER_H
#define NETHEL_TIMER_H
#include <stdint.h>

void     timer_init(uint32_t hz);
uint64_t timer_ticks(void);
void     timer_sleep(uint32_t ms);

#endif
