#include "gdt.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 5 descriptores normales + TSS (16 bytes = 2 entradas) */
static uint64_t gdt[7];
static struct tss tss;
static struct gdtr gdtr;

/* Stack inicial para entradas desde Ring 3 (luego se cambia por tarea) */
static uint8_t kstack[16384] __attribute__((aligned(16)));

void gdt_set_kernel_stack(uint64_t rsp0) { tss.rsp0 = rsp0; }

static void set_tss_desc(int idx, uint64_t base, uint32_t limit)
{
    gdt[idx] = (limit & 0xFFFFULL)
             | ((base & 0xFFFFFFULL) << 16)
             | (0x89ULL << 40)                       /* presente, TSS 64 disponible */
             | (((uint64_t)(limit >> 16) & 0xF) << 48)
             | (((base >> 24) & 0xFFULL) << 56);
    gdt[idx + 1] = base >> 32;
}

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* kernel code 64 */
    gdt[2] = 0x00CF92000000FFFFULL;   /* kernel data    */
    gdt[3] = 0x00CFF2000000FFFFULL;   /* user data (DPL 3) */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* user code 64 (DPL 3) */

    tss.rsp0 = (uint64_t)(kstack + sizeof(kstack));
    tss.iomap_base = sizeof(tss);     /* sin bitmap de I/O */
    set_tss_desc(5, (uint64_t)&tss, sizeof(tss) - 1);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)gdt;

    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        : : "m"(gdtr) : "rax", "memory");

    __asm__ volatile("ltr %w0" : : "r"((uint16_t)SEL_TSS));
}
