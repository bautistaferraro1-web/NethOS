#include "pmm.h"
#include "console.h"

/* vmm_init() mapea 4 GiB (identidad); el PMM maneja hasta 4 GiB */
#define MAX_MEM    (4ULL << 30)
#define MAX_PAGES  (MAX_MEM / PAGE_SIZE)

extern char kernel_end[];

struct mb2_tag { uint32_t type, size; };
struct mb2_mmap_tag { uint32_t type, size, entry_size, entry_version; };
struct mb2_mmap_entry { uint64_t base, length; uint32_t type, reserved; };

static uint8_t  bitmap[MAX_PAGES / 8];   /* 1 = usada, 0 = libre */
static uint64_t hint;
static uint64_t used_pages = MAX_PAGES;

static inline int  bit_get(uint64_t i) { return bitmap[i >> 3] & (1 << (i & 7)); }
static inline void bit_set(uint64_t i) { bitmap[i >> 3] |=  (1 << (i & 7)); }
static inline void bit_clr(uint64_t i) { bitmap[i >> 3] &= ~(1 << (i & 7)); }

static void mark_free(uint64_t base, uint64_t len)
{
    uint64_t start = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end   = (base + len) & ~(PAGE_SIZE - 1);
    if (end > MAX_MEM) end = MAX_MEM;
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t i = a / PAGE_SIZE;
        if (bit_get(i)) { bit_clr(i); used_pages--; }
    }
}

static void mark_used(uint64_t base, uint64_t len)
{
    uint64_t start = base & ~(PAGE_SIZE - 1);
    uint64_t end   = (base + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (end > MAX_MEM) end = MAX_MEM;
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t i = a / PAGE_SIZE;
        if (!bit_get(i)) { bit_set(i); used_pages++; }
    }
}

static const char *type_name(uint32_t t)
{
    switch (t) {
    case 1: return "AVAILABLE";
    case 2: return "RESERVED";
    case 3: return "ACPI RECLAIM";
    case 4: return "ACPI NVS";
    case 5: return "BAD";
    default: return "UNKNOWN";
    }
}

void pmm_init(uint64_t mb2_info)
{
    for (uint64_t i = 0; i < sizeof(bitmap); i++) bitmap[i] = 0xFF;

    uint32_t total_size = *(uint32_t *)mb2_info;
    uint64_t total_ram = 0, avail_ram = 0;

    console_puts("Memory map:\n");

    uint8_t *p = (uint8_t *)(mb2_info + 8);
    for (;;) {
        struct mb2_tag *t = (struct mb2_tag *)p;
        if (t->type == 0) break;
        if (t->type == 6) {
            struct mb2_mmap_tag *m = (struct mb2_mmap_tag *)t;
            uint8_t *e = (uint8_t *)m + sizeof(*m);
            uint8_t *end = (uint8_t *)m + m->size;
            for (; e < end; e += m->entry_size) {
                struct mb2_mmap_entry *en = (struct mb2_mmap_entry *)e;
                console_hex(en->base);
                console_puts(" - ");
                console_hex(en->base + en->length - 1);
                console_puts(" ");
                console_puts(type_name(en->type));
                console_putc('\n');

                if (en->type == 1 || en->type == 3 || en->type == 4) total_ram += en->length;
                if (en->type == 1) {
                    avail_ram += en->length;
                    mark_free(en->base, en->length);
                }
            }
        }
        p += (t->size + 7) & ~7u;
    }

    mark_used(0, 0x100000);
    mark_used(0x100000, (uint64_t)kernel_end - 0x100000);
    mark_used(mb2_info, total_size);

    console_puts("\nTotal RAM (map): "); console_dec(total_ram >> 20);
    console_puts(" MiB\nAvailable:       "); console_dec(avail_ram >> 20);
    console_puts(" MiB\nPMM free pages:  "); console_dec(pmm_free_pages());
    console_puts(" (");  console_dec((pmm_free_pages() * PAGE_SIZE) >> 20);
    console_puts(" MiB, limitado a 4 GiB)\n");
}

uint64_t pmm_alloc_page(void)
{
    for (uint64_t n = 0; n < MAX_PAGES; n++) {
        uint64_t i = (hint + n) % MAX_PAGES;
        if (!bit_get(i)) {
            bit_set(i);
            used_pages++;
            hint = i + 1;
            return i * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys)
{
    uint64_t i = phys / PAGE_SIZE;
    if (i < MAX_PAGES && bit_get(i)) {
        bit_clr(i);
        used_pages--;
        if (i < hint) hint = i;
    }
}

uint64_t pmm_free_pages(void)  { return MAX_PAGES - used_pages; }
uint64_t pmm_total_pages(void) { return MAX_PAGES; }

uint64_t pmm_alloc_contiguous(uint64_t n)
{
    uint64_t run = 0, start = 0;
    if (!n) return 0;
    for (uint64_t i = 0; i < MAX_PAGES; i++) {
        if (bit_get(i)) { run = 0; continue; }
        if (run == 0) start = i;
        if (++run == n) {
            for (uint64_t j = start; j < start + n; j++) bit_set(j);
            used_pages += n;
            return start * PAGE_SIZE;
        }
    }
    return 0;
}
