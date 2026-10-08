#include "progs.h"

extern const uint8_t _binary_hello_elf_start[], _binary_hello_elf_end[];
extern const uint8_t _binary_echo_elf_start[],  _binary_echo_elf_end[];
extern const uint8_t _binary_nsh_elf_start[],   _binary_nsh_elf_end[];
extern const uint8_t _binary_loop_elf_start[],  _binary_loop_elf_end[];
extern const uint8_t _binary_sse_elf_start[],   _binary_sse_elf_end[];
extern const uint8_t _binary_abi_elf_start[],   _binary_abi_elf_end[];
extern const uint8_t _binary_memt_elf_start[],  _binary_memt_elf_end[];

static const struct prog table[] = {
    { "hello", _binary_hello_elf_start, _binary_hello_elf_end },
    { "echo",  _binary_echo_elf_start,  _binary_echo_elf_end  },
    { "nsh",   _binary_nsh_elf_start,   _binary_nsh_elf_end   },
    { "loop",  _binary_loop_elf_start,  _binary_loop_elf_end  },
    { "sse",   _binary_sse_elf_start,   _binary_sse_elf_end   },
    { "abi",   _binary_abi_elf_start,   _binary_abi_elf_end   },
    { "memt",  _binary_memt_elf_start,  _binary_memt_elf_end  },
};
#define N ((int)(sizeof(table) / sizeof(table[0])))

int prog_count(void) { return N; }
const struct prog *prog_at(int i) { return (i >= 0 && i < N) ? &table[i] : 0; }

int prog_find(const char *name)
{
    for (int i = 0; i < N; i++) {
        const char *a = table[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return i;
    }
    return -1;
}
