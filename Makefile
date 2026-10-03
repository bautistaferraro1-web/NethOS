.RECIPEPREFIX = >
CC      = gcc
LD      = ld
EXTRA   =
CFLAGS  = -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -fno-tree-loop-distribute-patterns -Wall -Wextra -Iinclude $(EXTRA)
LDFLAGS = -n -nostdlib -z max-page-size=0x1000 -T arch/x86_64/boot/linker.ld

OBJS = build/boot.o build/isr.o build/main.o build/console.o build/pmm.o build/idt.o build/heap.o build/irq.o build/timer.o build/keyboard.o build/vmm.o build/switch.o build/sched.o build/gdt.o build/user.o build/syscall.o build/uprog.o build/elf.o build/hello_elf.o

all: build/nethel.elf

build/boot.o: arch/x86_64/boot/boot.S
> @mkdir -p build
> $(CC) -c $< -o $@

build/%.o: kernel/%.S
> @mkdir -p build
> $(CC) -c $< -o $@

build/%.o: kernel/%.c $(wildcard include/*.h)
> @mkdir -p build
> $(CC) $(CFLAGS) -c $< -o $@

build/nethel.elf: $(OBJS)
> $(LD) $(LDFLAGS) -o $@ $(OBJS)
> @file $@
> @grub-file --is-x86-multiboot2 $@ && echo "Multiboot2 OK" || true

iso: build/nethel.elf
> cp build/nethel.elf iso/boot/nethel.elf
> grub-mkrescue -o build/nethel.iso iso

run: iso
> qemu-system-x86_64 -cdrom build/nethel.iso -m 256M

clean:
> rm -rf build iso/boot/nethel.elf

.PHONY: all iso run clean

build/hello.o: user/hello.c Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/hello.c -o build/hello.o

build/hello.elf: build/hello.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/hello.elf build/hello.o

build/hello_elf.o: build/hello.elf
> cd build && ld -r -b binary -z noexecstack -o hello_elf.o hello.elf
