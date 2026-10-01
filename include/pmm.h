#ifndef NETHEL_PMM_H
#define NETHEL_PMM_H
#include <stdint.h>

#define PAGE_SIZE 4096ULL

void     pmm_init(uint64_t mb2_info);
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t phys);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);

#endif
