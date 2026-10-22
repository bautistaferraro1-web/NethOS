#include <stdint.h>
#include "pci.h"
#include "io.h"
#include "console.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_read32(uint8_t bus, uint8_t dev,
                           uint8_t fn, uint8_t reg)
{
    uint32_t address = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)dev << 11)
        | ((uint32_t)fn << 8)
        | ((uint32_t)reg & 0xFCu);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void print_bdf(uint8_t bus, uint8_t dev, uint8_t fn)
{
    console_dec(bus);
    console_putc(':');
    console_dec(dev);
    console_putc('.');
    console_dec(fn);
}

static void report_storage(uint8_t bus, uint8_t dev, uint8_t fn,
                           uint16_t vendor, uint16_t device,
                           uint8_t class_code, uint8_t subclass,
                           uint8_t prog_if, uint32_t bar0, uint32_t bar1)
{
    console_puts("[pci] almacenamiento ");
    print_bdf(bus, dev, fn);

    console_puts(" id=");
    console_hex(((uint64_t)vendor << 16) | device);

    console_puts(" clase=");
    console_dec(class_code);
    console_putc('/');
    console_dec(subclass);
    console_puts(" prog-if=");
    console_hex(prog_if);

    console_puts(" BAR0=");
    console_hex(bar0);
    console_puts(" BAR1=");
    console_hex(bar1);
    console_putc('\n');

    if (class_code != 0x01 || subclass != 0x08)
        return;

    console_puts("[pci] controlador NVMe en ");
    print_bdf(bus, dev, fn);
    console_putc('\n');

    if (bar0 == 0 || bar0 == 0xFFFFFFFFu) {
        console_puts("[pci] BAR0 no asignado\n");
        return;
    }

    if (bar0 & 1u) {
        console_puts("[pci] BAR0 es de E/S; no se inicializa\n");
        return;
    }

    uint32_t type = (bar0 >> 1) & 3u;
    uint64_t base = bar0 & ~0xFULL;

    if (type == 2u)
        base |= (uint64_t)bar1 << 32;

    console_puts("[pci] NVMe BAR0 MMIO base=");
    console_hex(base);
    console_puts(type == 2u ? " (64-bit)\n" : " (32-bit o reservado)\n");
}

void pci_init(void)
{
    unsigned found = 0;

    console_puts("\nPCI: buscando controladores de almacenamiento...\n");

    /*
     * PCI Configuration Mechanism #1.
     * Solo leemos la configuración; no cambiamos BARs ni Command Register.
     */
    for (unsigned b = 0; b < 256; b++) {
        for (unsigned d = 0; d < 32; d++) {
            uint32_t id0 = pci_read32((uint8_t)b, (uint8_t)d, 0, 0);
            if ((id0 & 0xFFFFu) == 0xFFFFu)
                continue;

            uint32_t header = pci_read32((uint8_t)b, (uint8_t)d, 0, 0x0C);
            unsigned functions = ((header >> 16) & 0x80u) ? 8u : 1u;

            for (unsigned f = 0; f < functions; f++) {
                uint32_t id = f == 0
                    ? id0
                    : pci_read32((uint8_t)b, (uint8_t)d,
                                 (uint8_t)f, 0);

                uint16_t ven = (uint16_t)(id & 0xFFFFu);
                if (ven == 0xFFFFu)
                    continue;

                uint16_t dev_id = (uint16_t)(id >> 16);
                uint32_t classreg = pci_read32(
                    (uint8_t)b, (uint8_t)d, (uint8_t)f, 0x08);

                uint8_t prog_if = (uint8_t)(classreg >> 8);
                uint8_t subclass = (uint8_t)(classreg >> 16);
                uint8_t class_code = (uint8_t)(classreg >> 24);

                if (class_code != 0x01)
                    continue;

                uint32_t bar0 = pci_read32(
                    (uint8_t)b, (uint8_t)d, (uint8_t)f, 0x10);
                uint32_t bar1 = pci_read32(
                    (uint8_t)b, (uint8_t)d, (uint8_t)f, 0x14);

                report_storage((uint8_t)b, (uint8_t)d, (uint8_t)f,
                               ven, dev_id, class_code, subclass,
                               prog_if, bar0, bar1);
                found++;
            }
        }
    }

    console_puts("PCI: controladores de almacenamiento encontrados: ");
    console_dec(found);
    console_putc('\n');
}

/* API para el controlador NVMe. */
static void pci_cfg_write16(uint8_t bus, uint8_t dev,
                            uint8_t fn, uint8_t reg, uint16_t value)
{
    uint32_t address = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)dev << 11)
        | ((uint32_t)fn << 8)
        | ((uint32_t)reg & 0xFCu);

    outl(0xCF8, address);
    outw(0xCFC + (reg & 2u), value);
}

int pci_find_nvme(struct pci_nvme_device *out)
{
    if (!out)
        return -1;

    for (unsigned b = 0; b < 256; b++) {
        for (unsigned d = 0; d < 32; d++) {
            uint32_t id0 = pci_read32((uint8_t)b, (uint8_t)d, 0, 0);
            if ((id0 & 0xFFFFu) == 0xFFFFu)
                continue;

            uint32_t header = pci_read32(
                (uint8_t)b, (uint8_t)d, 0, 0x0C);
            unsigned functions = ((header >> 16) & 0x80u) ? 8u : 1u;

            for (unsigned f = 0; f < functions; f++) {
                uint32_t id = f == 0
                    ? id0
                    : pci_read32((uint8_t)b, (uint8_t)d,
                                 (uint8_t)f, 0);

                uint16_t vendor = (uint16_t)id;
                if (vendor == 0xFFFFu)
                    continue;

                uint32_t classreg = pci_read32(
                    (uint8_t)b, (uint8_t)d, (uint8_t)f, 0x08);

                if ((uint8_t)(classreg >> 24) != 0x01u ||
                    (uint8_t)(classreg >> 16) != 0x08u)
                    continue;

                uint32_t bar0 = pci_read32(
                    (uint8_t)b, (uint8_t)d, (uint8_t)f, 0x10);
                uint32_t bar1 = pci_read32(
                    (uint8_t)b, (uint8_t)d, (uint8_t)f, 0x14);

                if (bar0 == 0 || bar0 == 0xFFFFFFFFu || (bar0 & 1u))
                    continue;

                unsigned type = (bar0 >> 1) & 3u;
                if (type != 0u && type != 2u)
                    continue;

                uint64_t base = (uint64_t)(bar0 & ~0xFu);
                if (type == 2u)
                    base |= (uint64_t)bar1 << 32;

                if (!base)
                    continue;

                out->bus = (uint8_t)b;
                out->device = (uint8_t)d;
                out->function = (uint8_t)f;
                out->vendor_id = vendor;
                out->device_id = (uint16_t)(id >> 16);
                out->bar0 = base;
                return 0;
            }
        }
    }

    return -1;
}

int pci_enable_nvme(const struct pci_nvme_device *dev)
{
    if (!dev)
        return -1;

    /* Habilitar Memory Space y Bus Master, preservando los demás bits. */
    uint32_t command_status = pci_read32(
        dev->bus, dev->device, dev->function, 0x04);
    uint16_t command = (uint16_t)command_status;
    command |= (1u << 1) | (1u << 2);

    pci_cfg_write16(dev->bus, dev->device, dev->function, 0x04,
                    command);
    return 0;
}
