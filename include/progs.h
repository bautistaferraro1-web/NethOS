#ifndef NETHEL_PROGS_H
#define NETHEL_PROGS_H
#include <stdint.h>

struct prog { const char *name; const uint8_t *start, *end; };

int                 prog_count(void);
const struct prog  *prog_at(int i);
int                 prog_find(const char *name);     /* indice o -1 */

#endif
