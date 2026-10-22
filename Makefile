.RECIPEPREFIX = >
CC      = gcc
LD      = ld
EXTRA   =
CFLAGS  = -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -fno-tree-loop-distribute-patterns -Wall -Wextra -Iinclude $(EXTRA)
LDFLAGS = -n -nostdlib -z max-page-size=0x1000 -T arch/x86_64/boot/linker.ld

OBJS = build/boot.o build/isr.o build/main.o build/console.o build/pmm.o build/pci.o build/nvme.o build/block.o build/partition.o build/fat32.o build/idt.o build/heap.o build/irq.o build/timer.o build/keyboard.o build/power.o build/vmm.o build/switch.o build/sched.o build/gdt.o build/user.o build/syscall.o build/signal.o build/file.o build/initramfs.o build/initramfs_cpio.o build/ftest_elf.o build/uprog.o build/elf.o build/hello_elf.o build/echo_elf.o build/progs.o build/nsh_elf.o build/loop_elf.o build/fpu.o build/sse_elf.o build/abi_elf.o build/memt_elf.o build/iotest_elf.o build/sctest_elf.o build/gtest_elf.o build/exectest_elf.o build/runsh_elf.o build/init_elf.o build/pipe.o build/tmpfs.o

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
> qemu-system-x86_64 -cdrom build/nethel.iso -m 256M -serial stdio

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

build/echo.o: user/echo.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/echo.c -o build/echo.o

build/echo.elf: build/echo.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/echo.elf build/echo.o

build/echo_elf.o: build/echo.elf
> cd build && ld -r -b binary -z noexecstack -o echo_elf.o echo.elf

build/nsh.o: user/nsh.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/nsh.c -o build/nsh.o

build/nsh.elf: build/nsh.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/nsh.elf build/nsh.o

build/nsh_elf.o: build/nsh.elf
> cd build && ld -r -b binary -z noexecstack -o nsh_elf.o nsh.elf

build/loop.o: user/loop.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/loop.c -o build/loop.o

build/loop.elf: build/loop.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/loop.elf build/loop.o

build/loop_elf.o: build/loop.elf
> cd build && ld -r -b binary -z noexecstack -o loop_elf.o loop.elf

build/sse.o: user/sse.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/sse.c -o build/sse.o

build/sse.elf: build/sse.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/sse.elf build/sse.o

build/sse_elf.o: build/sse.elf
> cd build && ld -r -b binary -z noexecstack -o sse_elf.o sse.elf

build/abi.o: user/abi.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/abi.c -o build/abi.o

build/abi.elf: build/abi.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/abi.elf build/abi.o

build/abi_elf.o: build/abi.elf
> cd build && ld -r -b binary -z noexecstack -o abi_elf.o abi.elf

build/memt.o: user/memt.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/memt.c -o build/memt.o

build/memt.elf: build/memt.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/memt.elf build/memt.o

build/memt_elf.o: build/memt.elf
> cd build && ld -r -b binary -z noexecstack -o memt_elf.o memt.elf

build/iotest.o: user/iotest.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/iotest.c -o build/iotest.o

build/iotest.elf: build/iotest.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/iotest.elf build/iotest.o

build/iotest_elf.o: build/iotest.elf
> cd build && ld -r -b binary -z noexecstack -o iotest_elf.o iotest.elf

build/sctest.o: user/sctest.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/sctest.c -o build/sctest.o

build/sctest.elf: build/sctest.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/sctest.elf build/sctest.o

build/sctest_elf.o: build/sctest.elf
> cd build && ld -r -b binary -z noexecstack -o sctest_elf.o sctest.elf

build/gtest.elf: prebuilt/gtest.elf
> mkdir -p build
> cp prebuilt/gtest.elf build/gtest.elf

build/gtest_elf.o: build/gtest.elf
> cd build && ld -r -b binary -z noexecstack -o gtest_elf.o gtest.elf

build/initramfs.o: kernel/initramfs.c include/initramfs.h include/file.h
>mkdir -p build
>gcc -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -Iinclude -c kernel/initramfs.c -o build/initramfs.o

build/reboot.o: user/reboot.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -Iinclude -c user/reboot.c -o build/reboot.o

build/reboot.elf: build/reboot.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/reboot.elf build/reboot.o

initrd/bin/reboot: build/reboot.elf
> rm -f $@
> cp $< $@

build/poweroff.o: user/poweroff.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -Iinclude -c user/poweroff.c -o build/poweroff.o

build/poweroff.elf: build/poweroff.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/poweroff.elf build/poweroff.o

initrd/bin/poweroff: build/poweroff.elf
> rm -f $@
> cp $< $@

build/initramfs.cpio: $(shell find initrd \( -type f -o -type l \)) Makefile initrd/bin/reboot initrd/bin/poweroff
>mkdir -p build
>cd initrd && find . -mindepth 1 | cpio -o -H newc --quiet > ../build/initramfs.cpio

build/initramfs_cpio.o: build/initramfs.cpio
>cd build && ld -r -b binary -z noexecstack -o initramfs_cpio.o initramfs.cpio

build/ftest.o: user/ftest.c user/nlib.h Makefile
>mkdir -p build
>gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/ftest.c -o build/ftest.o

build/ftest.elf: build/ftest.o user/user.ld
>ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/ftest.elf build/ftest.o

build/ftest_elf.o: build/ftest.elf
>cd build && ld -r -b binary -z noexecstack -o ftest_elf.o ftest.elf

build/exectest.o: user/exectest.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/exectest.c -o build/exectest.o

build/exectest.elf: build/exectest.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/exectest.elf build/exectest.o

build/exectest_elf.o: build/exectest.elf
> cd build && ld -r -b binary -z noexecstack -o exectest_elf.o exectest.elf

build/runsh.o: user/runsh.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/runsh.c -o build/runsh.o

build/runsh.elf: build/runsh.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/runsh.elf build/runsh.o

build/runsh_elf.o: build/runsh.elf
> cd build && ld -r -b binary -z noexecstack -o runsh_elf.o runsh.elf

build/pipe.o: kernel/pipe.c include/file.h include/heap.h
>gcc -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -Iinclude -c kernel/pipe.c -o build/pipe.o

build/tmpfs.o: kernel/tmpfs.c include/tmpfs.h include/file.h
>gcc -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -Iinclude -c kernel/tmpfs.c -o build/tmpfs.o

build/init.o: user/init.c user/nlib.h Makefile
> mkdir -p build
> gcc -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -c user/init.c -o build/init.o

build/init.elf: build/init.o user/user.ld
> ld -nostdlib -z max-page-size=0x1000 -T user/user.ld -o build/init.elf build/init.o

build/init_elf.o: build/init.elf
> cd build && ld -r -b binary -z noexecstack -o init_elf.o init.elf
