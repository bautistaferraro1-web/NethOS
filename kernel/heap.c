#include "heap.h"
#include "cpu.h"
#include "pmm.h"
#include "console.h"

#define MAGIC 0x4E455448u            /* "NETH" */
#define ALIGN 16

struct block {
    uint64_t size;                   /* bytes utiles (sin el header) */
    struct block *prev, *next;       /* lista ordenada por direccion */
    uint32_t free;
    uint32_t magic;
};                                   /* 32 bytes: mantiene alineacion de 16 */

#define HDR sizeof(struct block)

static struct block *head;
static uint64_t heap_base, heap_total;

void heap_init(uint64_t pages)
{
    uint64_t base = pmm_alloc_contiguous(pages);
    if (!base) {
        console_puts("heap: no hay memoria contigua\n");
        return;
    }
    heap_base  = base;
    heap_total = pages * PAGE_SIZE;

    head = (struct block *)base;
    head->size  = heap_total - HDR;
    head->prev  = head->next = 0;
    head->free  = 1;
    head->magic = MAGIC;
}

static void *kmalloc_raw(uint64_t size)
{
    if (!size || !head) return 0;
    size = (size + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);

    for (struct block *b = head; b; b = b->next) {
        if (!b->free || b->size < size) continue;

        if (b->size >= size + HDR + ALIGN) {          /* partir el bloque */
            struct block *n = (struct block *)((uint8_t *)b + HDR + size);
            n->size  = b->size - size - HDR;
            n->free  = 1;
            n->magic = MAGIC;
            n->prev  = b;
            n->next  = b->next;
            if (b->next) b->next->prev = n;
            b->next = n;
            b->size = size;
        }
        b->free = 0;
        return (uint8_t *)b + HDR;
    }
    return 0;
}

static void kfree_raw(void *p)
{
    if (!p) return;

    uint64_t a = (uint64_t)p;
    if (a < heap_base + HDR || a >= heap_base + heap_total) {
        console_puts("kfree: puntero fuera del heap\n");
        return;
    }

    struct block *b = (struct block *)((uint8_t *)p - HDR);
    if (b->magic != MAGIC) { console_puts("kfree: puntero invalido o heap corrupto\n"); return; }
    if (b->free)           { console_puts("kfree: double free\n"); return; }

    b->free = 1;

    if (b->next && b->next->free) {                   /* fusionar con el siguiente */
        b->size += HDR + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {                   /* fusionar con el anterior */
        b->prev->size += HDR + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

uint64_t heap_used(void)
{
    uint64_t n = 0;
    for (struct block *b = head; b; b = b->next) if (!b->free) n += b->size;
    return n;
}

uint64_t heap_free(void)
{
    uint64_t n = 0;
    for (struct block *b = head; b; b = b->next) if (b->free) n += b->size;
    return n;
}

void *kmalloc(uint64_t size)
{
    uint64_t f = irq_save();
    void *p = kmalloc_raw(size);
    irq_restore(f);
    return p;
}

void kfree(void *p)
{
    uint64_t f = irq_save();
    kfree_raw(p);
    irq_restore(f);
}
