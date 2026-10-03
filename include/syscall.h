#ifndef NETHEL_SYSCALL_H
#define NETHEL_SYSCALL_H
#include "idt.h"

void syscall_dispatch(struct regs *r);

#endif
