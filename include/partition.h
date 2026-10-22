#ifndef NETHEL_PARTITION_H
#define NETHEL_PARTITION_H

#include <stdint.h>

#define PARTITION_SCHEME_GPT 1u
#define PARTITION_SCHEME_MBR 2u

struct partition_info {
    uint32_t device;
    uint32_t scheme;
    uint32_t index;
    uint8_t  mbr_type;
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t sector_count;
    uint64_t attributes;
    char name[37];
};

void partition_init(void);
uint32_t partition_count(void);
int partition_get(uint32_t index, struct partition_info *out);

#endif
