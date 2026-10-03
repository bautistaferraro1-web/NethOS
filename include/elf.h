#ifndef NETHEL_ELF_H
#define NETHEL_ELF_H
#include <stdint.h>

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

/* Carga los PT_LOAD de 'image' en 'space' (solo dentro de [lo, hi)).
   Devuelve 0 y el punto de entrada, o -1. Llamar con interrupciones desactivadas. */
int elf_load(const void *image, uint64_t size, uint64_t space,
             uint64_t lo, uint64_t hi, uint64_t *entry);

#endif
