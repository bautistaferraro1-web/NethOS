#include <stdint.h>

#include "block.h"
#include "nvme.h"
#include "console.h"

#define BLOCK_MAX_DEVICES 4
#define BLOCK_MAX_TRANSFER 4096u

struct block_device {
    uint64_t sectors;
    uint32_t sector_size;
    int present;
};

static struct block_device devices[BLOCK_MAX_DEVICES];
static uint32_t device_count;

void block_init(void)
{
    uint64_t sectors = 0;
    uint32_t sector_size = 0;

    device_count = 0;

    if (nvme_get_info(&sectors, &sector_size) != 0) {
        console_puts("BLOCK: no hay dispositivos disponibles\n");
        return;
    }

    if (!sectors || !sector_size ||
        sector_size > BLOCK_MAX_TRANSFER ||
        BLOCK_MAX_TRANSFER % sector_size != 0) {
        console_puts("BLOCK: geometria del dispositivo no soportada\n");
        return;
    }

    devices[0].sectors = sectors;
    devices[0].sector_size = sector_size;
    devices[0].present = 1;
    device_count = 1;

    console_puts("BLOCK: dispositivo 0 registrado (NVMe)\n");
    console_puts("BLOCK: sectores=");
    console_dec(sectors);
    console_puts(", bytes/sector=");
    console_dec(sector_size);
    console_putc('\n');
}

uint32_t block_device_count(void)
{
    return device_count;
}

int block_get_info(uint32_t device,
                   uint64_t *sector_count,
                   uint32_t *sector_size)
{
    if (device >= device_count || !devices[device].present ||
        !sector_count || !sector_size)
        return -1;

    *sector_count = devices[device].sectors;
    *sector_size = devices[device].sector_size;
    return 0;
}

static int block_validate(uint32_t device, uint64_t lba,
                          uint32_t count, const void *buffer)
{
    if (device >= device_count || !devices[device].present ||
        !buffer || !count)
        return -1;

    const struct block_device *d = &devices[device];

    if (lba >= d->sectors || count > d->sectors - lba)
        return -1;

    /* El controlador NVMe actual usa un buffer DMA de una sola página. */
    if (count > BLOCK_MAX_TRANSFER / d->sector_size)
        return -1;

    return 0;
}

int block_read(uint32_t device, uint64_t lba,
               uint32_t count, void *buffer)
{
    if (block_validate(device, lba, count, buffer) != 0)
        return -1;

    return nvme_read_blocks(lba, count, buffer);
}

int block_write(uint32_t device, uint64_t lba,
                uint32_t count, const void *buffer)
{
    if (block_validate(device, lba, count, buffer) != 0)
        return -1;

    return nvme_write_blocks(lba, count, buffer);
}
