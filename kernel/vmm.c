#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "mem.h"

#define ADDR_MASK   0x000FFFFFFFFFF000ULL
#define HUGE_MASK   0x000FFFFFFFE00000ULL      /* paginas de 2 MiB */
#define GIB_MASK    0x000FFFFFC0000000ULL      /* paginas de 1 GiB */
#define IDX(v, lvl) (((v) >> (12 + 9 * (lvl))) & 0x1FF)   /* 0=PT 1=PD 2=PDPT 3=PML4 */
#define DIRECT_MAP_GB 4

#ifdef VMM_HOST                                 /* solo para tests en el host */
#define invlpg(v)    ((void)(v))
#define load_cr3(p)  ((void)(p))
#else
static inline void invlpg(uint64_t v)   { __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory"); }
static inline void load_cr3(uint64_t p) { __asm__ volatile("mov %0, %%cr3" : : "r"(p) : "memory"); }
#endif

static uint64_t *kpml4;                         /* espacio del kernel */
static uint64_t *cur;                           /* espacio activo (el que esta en CR3) */

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
    uint64_t *t = (uint64_t *)P2V(p);
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

    pd[idx] = V2P(pt) | VMM_PRESENT | VMM_WRITE | (e & VMM_USER);
    return pt;
}

static uint64_t *descend(uint64_t *tbl, uint64_t idx, int create, uint64_t flags)
{
    uint64_t e = tbl[idx];
    if (e & VMM_PRESENT) {
        if (e & VMM_HUGE) return 0;
        if ((flags & VMM_USER) && !(e & VMM_USER)) tbl[idx] = e | VMM_USER;
        return (uint64_t *)P2V(e & ADDR_MASK);
    }
    if (!create) return 0;

    uint64_t *n = table_alloc();
    if (!n) return 0;
    tbl[idx] = V2P(n) | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
    return n;
}

static uint64_t *get_pt(uint64_t *root, uint64_t virt, int create, uint64_t flags)
{
    uint64_t *pdpt = descend(root, IDX(virt, 3), create, flags);
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

static int map_in(uint64_t *root, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!valid_virt(virt) || (phys & 0xFFF)) return -1;
    /* La mitad alta (pml4[256..511]) es compartida: tocarla desde un espacio de usuario
       partiria paginas de 2 MiB que usan todos los procesos y el kernel. */
    if (root != kpml4 && IDX(virt, 3) >= 256) return -1;
    uint64_t *pt = get_pt(root, virt, 1, flags);
    if (!pt) return -1;
    pt[IDX(virt, 0)] = (phys & ADDR_MASK) | (flags & 0xFFF) | VMM_PRESENT;
    invlpg(virt);
    return 0;
}

static int unmap_in(uint64_t *root, uint64_t virt)
{
    if (!valid_virt(virt)) return -1;
    uint64_t *pt = get_pt(root, virt, 0, 0);
    if (!pt) return -1;
    uint64_t i = IDX(virt, 0);
    if (!(pt[i] & VMM_PRESENT)) return -1;
    pt[i] = 0;
    invlpg(virt);
    return 0;
}

static int translate_in(uint64_t *root, uint64_t virt, uint64_t *phys)
{
    uint64_t e = root[IDX(virt, 3)];
    if (!(e & VMM_PRESENT)) return -1;

    e = ((uint64_t *)P2V(e & ADDR_MASK))[IDX(virt, 2)];
    if (!(e & VMM_PRESENT)) return -1;
    if (e & VMM_HUGE) { *phys = (e & GIB_MASK) | (virt & 0x3FFFFFFFULL); return 0; }

    e = ((uint64_t *)P2V(e & ADDR_MASK))[IDX(virt, 1)];
    if (!(e & VMM_PRESENT)) return -1;
    if (e & VMM_HUGE) { *phys = (e & HUGE_MASK) | (virt & 0x1FFFFFULL); return 0; }

    e = ((uint64_t *)P2V(e & ADDR_MASK))[IDX(virt, 0)];
    if (!(e & VMM_PRESENT)) return -1;
    *phys = (e & ADDR_MASK) | (virt & 0xFFF);
    return 0;
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) { return map_in(kpml4, virt, phys, flags); }
int vmm_unmap(uint64_t virt)                              { return unmap_in(kpml4, virt); }
int vmm_translate(uint64_t virt, uint64_t *phys)          { return translate_in(cur, virt, phys); }

int vmm_map_in(uint64_t space, uint64_t virt, uint64_t phys, uint64_t flags)
{
    return map_in((uint64_t *)P2V(space), virt, phys, flags);
}

uint64_t vmm_kernel_space(void) { return V2P(kpml4); }

void vmm_switch(uint64_t space)
{
    if (space == V2P(cur)) return;
    cur = (uint64_t *)P2V(space);
    load_cr3(space);
}

uint64_t vmm_create_space(void)
{
    uint64_t *p = table_alloc();
    if (!p) return 0;
    p[256] = kpml4[256];                        /* direct map compartido */
    p[511] = kpml4[511];                        /* kernel en la mitad alta, compartido */
    return V2P(p);
}

/* level: 0 = PT (sus entradas son frames), 1 = PD, 2 = PDPT */
static void free_tree(uint64_t tphys, int level)
{
    uint64_t *t = (uint64_t *)P2V(tphys);
    for (int i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & VMM_PRESENT)) continue;
        if (level == 0) pmm_free_page(e & ADDR_MASK);
        else if (!(e & VMM_HUGE)) free_tree(e & ADDR_MASK, level - 1);
    }
    pmm_free_page(tphys);
}

void vmm_destroy_space(uint64_t space)
{
    if (!space || space == V2P(kpml4)) return;
    if (space == V2P(cur)) vmm_switch(V2P(kpml4));

    uint64_t *root = (uint64_t *)P2V(space);
    for (int i = 0; i < 256; i++)   /* 256+ son del kernel */
                      /* la mitad baja es del proceso */
        if (root[i] & VMM_PRESENT) free_tree(root[i] & ADDR_MASK, 2);
    pmm_free_page(space);
}

void vmm_init(void)
{
    kpml4 = table_alloc();
    uint64_t *hdpt  = table_alloc();          /* direct map alto */
    uint64_t *kpdpt = table_alloc();          /* kernel en la mitad alta */
    if (!kpml4 || !hdpt || !kpdpt) vmm_panic("sin memoria para las tablas");

    for (uint64_t g = 0; g < DIRECT_MAP_GB; g++) {          /* 4 GiB, paginas de 2 MiB */
        uint64_t *pd = table_alloc();
        if (!pd) vmm_panic("sin memoria para las tablas");
        hdpt[g] = V2P(pd) | VMM_PRESENT | VMM_WRITE;
        for (uint64_t i = 0; i < 512; i++)
            pd[i] = ((g << 30) | (i << 21)) | VMM_PRESENT | VMM_WRITE | VMM_HUGE;
    }
    kpml4[256] = V2P(hdpt) | VMM_PRESENT | VMM_WRITE;       /* 0xFFFF800000000000 */

    /* Kernel en 0xFFFFFFFF80000000: pdpt[510] -> mismas tablas de 2 MiB (0..1 GiB) */
    kpdpt[510] = hdpt[0];
    kpml4[511] = V2P(kpdpt) | VMM_PRESENT | VMM_WRITE;

    /* pml4[0..255] queda vacio: la mitad baja es toda de usuario (y NULL no esta mapeado) */
    load_cr3(V2P(kpml4));
    cur = kpml4;
}

#ifndef VMM_HOST
void vmm_selftest(void)
{
    const uint64_t virt = 0x10000000000ULL;                 /* 1 TiB */
    const uint64_t magic = 0xDEADBEEFCAFEBABEULL;

    console_puts("\nVMM: direct map 4 GiB + kernel en la mitad alta\n");

    uint64_t phys = pmm_alloc_page();
    if (!phys || vmm_map(virt, phys, VMM_WRITE)) {
        console_puts("VMM: vmm_map fallo\n");
        return;
    }

    *(volatile uint64_t *)virt = magic;                     /* escribo por la direccion virtual */
    uint64_t alias = *(volatile uint64_t *)P2V(phys);            /* leo por la fisica (identidad) */
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
