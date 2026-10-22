#ifndef NETHEL_PCI_H
#define NETHEL_PCI_H

#include <stdint.h>

struct pci_nvme_device {
    uint8_t bus, device, function;
    uint16_t vendor_id, device_id;
    uint64_t bar0;
};

void pci_init(void);
int pci_find_nvme(struct pci_nvme_device *out);
int pci_enable_nvme(const struct pci_nvme_device *dev);

#endif
