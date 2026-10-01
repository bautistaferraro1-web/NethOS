#include "vmm.h"
#include "pmm.h"
#include "console.h"

#define ADDR_MASK   0x000FFFFFFFFFF000ULL
#define HUGE_MASK   0x000FFFFFFFE00000ULL      /* paginas de 2 MiB */
#define GIB_MASK    0x000FFFFFC0000000ULL      /* paginas de 1 GiB */
#define IDX(v, lvl) (((v) >> (12 + 9 * (lvl))) & 0x1FF)   /* 0=PT 1=PD 2=PDPT 3=PML4 */
#define IDENTITY_GB 4

#ifdef VMM_HOST                                 /* solo para tests en el host */
#define invlpg(v)    ((void)(v))
#define load_cr3(p)  ((void)(p))
#else
static inline void invlpg(uint64_t v)   { __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory"); }
static inline void load_cr3(uint64_t p) { __asm__ volatile("mov %0, %%cr3" : : "r"(p) : "memory"); }
#endif

static uint64_t *pml4;

static void vmm_panic(const char *msg)
{
    console_puts("VMM PANIC: ");
    console_puts(msg);
    console_putc('\n');
    for (;;) __asm__ volatile("cli; hlt");
}

static uint64_t *table_alloc(void)
{
    uint64_t p = pmm_alloc_page();
    if (!p) return 0;
    uint64_t *t = (uint64_t *)p;
    for (int i = 0; i < 512; i++) t[i] = 0;
    return t;
}

/* Convierte una pagina de 2 MiB en una tabla de 512 paginas de 4 KiB equivalentes */
static uint64_t *split_huge(uint64_t *pd, uint64_t idx)
{
    uint64_t e = pd[idx];
    uint64_t *pt = table_alloc();
    if (!pt) return 0;

    uint64_t base  = e & HUGE_MASK;
    uint64_t flags = e & 0xFFF & ~VMM_HUGE;
    for (uint64_t i = 0; i < 512; i++) pt[i] = (base + i * PAGE_SIZE) | flags;

    pd[idx] = (uint64_t)pt | VMM_PRESENT | VMM_WRITE | (e & VMM_USER);
    return pt;
}

static uint64_t *descend(uint64_t *tbl, uint64_t idx, int create, uint64_t flags)
{
    uint64_t e = tbl[idx];
    if (e & VMM_PRESENT) {
        if (e & VMM_HUGE) return 0;
        if ((flags & VMM_USER) && !(e & VMM_USER)) tbl[idx] = e | VMM_USER;
        return (uint64_t *)(e & ADDR_MASK);
    }
    if (!create) return 0;

    uint64_t *n = table_alloc();
    if (!n) return 0;
    tbl[idx] = (uint64_t)n | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
    return n;
}

static uint64_t *get_pt(uint64_t virt, int create, uint64_t flags)
{
    uint64_t *pdpt = descend(pml4, IDX(virt, 3), create, flags);
    if (!pdpt) return 0;
    uint64_t *pd = descend(pdpt, IDX(virt, 2), create, flags);
    if (!pd) return 0;

    uint64_t i = IDX(virt, 1);
    if ((pd[i] & VMM_PRESENT) && (pd[i] & VMM_HUGE)) return split_huge(pd, i);
    return descend(pd, i, create, flags);
}

static int valid_virt(uint64_t v)
{
    uint64_t top = v >> 47;                     /* direccion canonica */
    return (v & 0xFFF) == 0 && (top == 0 || top == 0x1FFFF);
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!valid_virt(virt) || (phys & 0xFFF)) return -1;
    uint64_t *pt = get_pt(virt, 1, flags);
    if (!pt) return -1;
    pt[IDX(virt, 0)] = (phys & ADDR_MASK) | (flags & 0xFFF) | VMM_PRESENT;
    invlpg(virt);
    return 0;
}

int vmm_unmap(uint64_t virt)
{
    if (!valid_virt(virt)) return -1;
    uint64_t *pt = get_pt(virt, 0, 0);
    if (!pt) return -1;
    uint64_t i = IDX(virt, 0);
    if (!(pt[i] & VMM_PRESENT)) return -1;
    pt[i] = 0;
    invlpg(virt);
    return 0;
}

int vmm_translate(uint64_t virt, uint64_t *phys)
{
    uint64_t e = pml4[IDX(virt, 3)];
    if (!(e & VMM_PRESENT)) return -1;

    e = ((uint64_t *)(e & ADDR_MASK))[IDX(virt, 2)];
    if (!(e & VMM_PRESENT)) return -1;
    if (e & VMM_HUGE) { *phys = (e & GIB_MASK) | (virt & 0x3FFFFFFFULL); return 0; }

    e = ((uint64_t *)(e & ADDR_MASK))[IDX(virt, 1)];
    if (!(e & VMM_PRESENT)) return -1;
    if (e & VMM_HUGE) { *phys = (e & HUGE_MASK) | (virt & 0x1FFFFFULL); return 0; }

    e = ((uint64_t *)(e & ADDR_MASK))[IDX(virt, 0)];
    if (!(e & VMM_PRESENT)) return -1;
    *phys = (e & ADDR_MASK) | (virt & 0xFFF);
    return 0;
}

void vmm_init(void)
{
    pml4 = table_alloc();
    uint64_t *pdpt = table_alloc();
    if (!pml4 || !pdpt) vmm_panic("sin memoria para las tablas");

    pml4[0] = (uint64_t)pdpt | VMM_PRESENT | VMM_WRITE;

    for (uint64_t g = 0; g < IDENTITY_GB; g++) {            /* 4 GiB identidad, paginas de 2 MiB */
        uint64_t *pd = table_alloc();
        if (!pd) vmm_panic("sin memoria para las tablas");
        pdpt[g] = (uint64_t)pd | VMM_PRESENT | VMM_WRITE;
        for (uint64_t i = 0; i < 512; i++)
            pd[i] = ((g << 30) | (i << 21)) | VMM_PRESENT | VMM_WRITE | VMM_HUGE;
    }

    load_cr3((uint64_t)pml4);

    if (vmm_unmap(0)) vmm_panic("no pude desmapear la pagina 0");   /* guard page: NULL -> #PF */
}

#ifndef VMM_HOST
void vmm_selftest(void)
{
    const uint64_t virt = 0x10000000000ULL;                 /* 1 TiB */
    const uint64_t magic = 0xDEADBEEFCAFEBABEULL;

    console_puts("\nVMM: identidad 4 GiB, pagina 0 desmapeada\n");

    uint64_t phys = pmm_alloc_page();
    if (!phys || vmm_map(virt, phys, VMM_WRITE)) {
        console_puts("VMM: vmm_map fallo\n");
        return;
    }

    *(volatile uint64_t *)virt = magic;                     /* escribo por la direccion virtual */
    uint64_t alias = *(volatile uint64_t *)phys;            /* leo por la fisica (identidad) */
    uint64_t t = 0;
    int rc = vmm_translate(virt, &t);

    console_puts("map "); console_hex(virt);
    console_puts(" -> "); console_hex(t);
    console_puts(rc == 0 && t == phys && alias == magic ? "  OK\n" : "  FALLO\n");

    vmm_unmap(virt);
    console_puts(vmm_translate(virt, &t) != 0 ? "unmap OK\n" : "unmap FALLO\n");

#ifdef TEST_UNMAPPED
    console_puts("Leyendo pagina desmapeada...\n");
    (void)*(volatile uint64_t *)virt;
#endif
#ifdef TEST_NULL
    console_puts("Dereferenciando NULL...\n");
    volatile uint64_t zero = 0;
    (void)*(volatile uint64_t *)zero;
#endif
    pmm_free_page(phys);
}
#endif
