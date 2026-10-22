#include <stdint.h>
#include "io.h"
#include "power.h"

/*
 * Reinicio x86:
 * 1. Intentar el controlador 8042.
 * 2. Intentar el registro de reset PCI 0xCF9.
 * 3. Como último recurso, provocar un triple fault.
 */
void power_reboot(void)
{
    __asm__ volatile("cli" : : : "memory");

    for (uint32_t i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 0x02)) {
            outb(0x64, 0xFE);
            break;
        }
    }

    for (volatile uint32_t i = 0; i < 1000000; i++)
        __asm__ volatile("pause");

    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);

    for (volatile uint32_t i = 0; i < 1000000; i++)
        __asm__ volatile("pause");

    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } null_idt = { 0, 0 };

    __asm__ volatile("lidt %0; int3" : : "m"(null_idt) : "memory");

    for (;;) __asm__ volatile("cli; hlt");
}

/*
 * Apagado inicial para máquinas virtuales compatibles con QEMU/Bochs.
 * Esto NO sustituye una implementación ACPI general para hardware real.
 */
void power_poweroff(void)
{
    __asm__ volatile("cli" : : : "memory");

    outw(0x0604, 0x2000);  /* QEMU con chipset PC compatible */
    for (volatile uint32_t i = 0; i < 1000000; i++)
        __asm__ volatile("pause");

    outw(0xB004, 0x2000);   /* compatibilidad con Bochs antiguo */

    for (;;) __asm__ volatile("cli; hlt");
}
