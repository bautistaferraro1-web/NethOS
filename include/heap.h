#ifndef NETHEL_HEAP_H
#define NETHEL_HEAP_H
#include <stdint.h>

void     heap_init(uint64_t pages);
void    *kmalloc(uint64_t size);
void     kfree(void *p);
uint64_t heap_used(void);
uint64_t heap_free(void);

#endif
