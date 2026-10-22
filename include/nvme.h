#ifndef NETHEL_NVME_H
#define NETHEL_NVME_H

#include <stdint.h>

void nvme_init(void);

/*
 * API del kernel. count se expresa en bloques lógicos.
 * Devuelven 0 en éxito y -1 en error.
 * Máximo actual: un bloque DMA de 4096 bytes por operación.
 */
int nvme_read_blocks(uint64_t lba, uint32_t count, void *buffer);
int nvme_write_blocks(uint64_t lba, uint32_t count, const void *buffer);

#ifdef NVME_TEST_WRITES
void nvme_selftest_write(void);
#endif

#endif
