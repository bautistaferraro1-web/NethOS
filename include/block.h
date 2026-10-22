#ifndef NETHEL_BLOCK_H
#define NETHEL_BLOCK_H

#include <stdint.h>

void block_init(void);

uint32_t block_device_count(void);

int block_get_info(uint32_t device,
                   uint64_t *sector_count,
                   uint32_t *sector_size);

int block_read(uint32_t device, uint64_t lba,
               uint32_t count, void *buffer);

int block_write(uint32_t device, uint64_t lba,
                uint32_t count, const void *buffer);

#endif
