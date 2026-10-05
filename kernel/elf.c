#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "mem.h"
#include "console.h"

#define PT_LOAD   1
#define PF_W      2
#define ET_EXEC   2
#define EM_X86_64 62
#define MAX_PH    16

static int bad(const char *m)
{
    console_puts("elf: "); console_puts(m); console_putc('\n');
    return -1;
}

int elf_load(const void *image, uint64_t size, uint64_t space,
             uint64_t lo, uint64_t hi, uint64_t *entry)
{
    const uint8_t *img = image;
    const struct elf64_ehdr *eh = image;

    if (size < sizeof(*eh)) return bad("archivo muy chico");
    if (img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F') return bad("magic invalido");
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1) return bad("no es ELF64 little-endian");
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) return bad("no es un ejecutable x86_64");
    if (eh->e_phentsize != sizeof(struct elf64_phdr) || eh->e_phnum == 0 || eh->e_phnum > MAX_PH)
        return bad("program headers invalidos");

    uint64_t phbytes = (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr);
    if (eh->e_phoff > size || phbytes > size - eh->e_phoff) return bad("program headers fuera del archivo");
    if (eh->e_entry < lo || eh->e_entry >= hi) return bad("entry fuera de la region de usuario");

    uint64_t next_free = lo;                    /* rechaza segmentos solapados o desordenados */
    int nload = 0;

    for (int i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)(img + eh->e_phoff) + i;
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        if (ph->p_filesz > ph->p_memsz) return bad("filesz > memsz");
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset) return bad("segmento fuera del archivo");
        if (ph->p_vaddr < lo || ph->p_vaddr >= hi || ph->p_memsz > hi - ph->p_vaddr)
            return bad("segmento fuera de la region de usuario");

        uint64_t first = ph->p_vaddr & ~0xFFFULL;
        uint64_t last  = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        if (first < next_free) return bad("segmentos solapados");
        next_free = last;

        uint64_t flags  = VMM_USER | ((ph->p_flags & PF_W) ? VMM_WRITE : 0);
        uint64_t fstart = ph->p_vaddr, fend = ph->p_vaddr + ph->p_filesz;

        for (uint64_t va = first; va < last; va += PAGE_SIZE) {
            uint64_t frame = pmm_alloc_page();
            if (!frame) return bad("sin memoria");

            volatile uint8_t *dst = (volatile uint8_t *)P2V(frame);   /* identidad */
            for (uint64_t k = 0; k < PAGE_SIZE; k++) dst[k] = 0;

            uint64_t s = va > fstart ? va : fstart;
            uint64_t e = (va + PAGE_SIZE) < fend ? (va + PAGE_SIZE) : fend;
            for (uint64_t a = s; a < e; a++)
                dst[a - va] = img[ph->p_offset + (a - fstart)];

            if (vmm_map_in(space, va, frame, flags)) {
                pmm_free_page(frame);
                return bad("vmm_map_in fallo");
            }
        }
        nload++;
    }

    if (!nload) return bad("sin segmentos PT_LOAD");
    *entry = eh->e_entry;
    return 0;
}
