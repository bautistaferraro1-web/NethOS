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

/* Stack de kernel para la instruccion syscall (igual a tss.rsp0; un solo CPU) */
uint64_t syscall_kstack, syscall_ustack;
extern void syscall_entry(void);

void gdt_set_kernel_stack(uint64_t rsp0) { tss.rsp0 = rsp0; syscall_kstack = rsp0; }

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

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

    /* syscall/sysret: STAR[47:32]=CS kernel (SS=+8), STAR[63:48]=base de sysret (SS=+8, CS=+16) */
    syscall_kstack = tss.rsp0;
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);                    /* EFER.SCE */
    wrmsr(0xC0000081, (0x10ULL << 48) | ((uint64_t)SEL_KCODE << 32));
    wrmsr(0xC0000082, (uint64_t)syscall_entry);                  /* LSTAR */
    wrmsr(0xC0000084, 0x700);                                    /* SFMASK: limpia TF, IF, DF */
}
