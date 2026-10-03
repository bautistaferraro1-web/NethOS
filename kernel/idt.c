#include "idt.h"
#include "console.h"
#include "irq.h"
#include "syscall.h"
#include "sched.h"
#include "user.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern uint64_t isr_table[48];
extern void isr128(void);
static struct idt_entry idt[256];

static const char *names[32] = {
    "Divide Error (#DE)", "Debug (#DB)", "NMI", "Breakpoint (#BP)",
    "Overflow (#OF)", "Bound Range (#BR)", "Invalid Opcode (#UD)",
    "Device Not Available (#NM)", "Double Fault (#DF)", "Coprocessor Overrun",
    "Invalid TSS (#TS)", "Segment Not Present (#NP)", "Stack Fault (#SS)",
    "General Protection (#GP)", "Page Fault (#PF)", "Reserved",
    "x87 FPU Error (#MF)", "Alignment Check (#AC)", "Machine Check (#MC)",
    "SIMD FP Exception (#XM)", "Virtualization (#VE)", "Control Protection (#CP)",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security (#SX)", "Reserved"
};

static void set_gate(int n, uint64_t handler)
{
    idt[n].off_lo  = handler & 0xFFFF;
    idt[n].sel     = 0x08;          /* selector de código de boot.S */
    idt[n].ist     = 0;
    idt[n].flags   = 0x8E;          /* present, DPL 0, interrupt gate */
    idt[n].off_mid = (handler >> 16) & 0xFFFF;
    idt[n].off_hi  = handler >> 32;
    idt[n].zero    = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++) set_gate(i, isr_table[i]);
    set_gate(0x80, (uint64_t)isr128);
    idt[0x80].flags = 0xEE;          /* DPL 3: Ring 3 puede hacer int 0x80 */

    struct idt_ptr p = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" : : "m"(p));
}

static void row(const char *name, uint64_t v)
{
    console_puts(name);
    console_hex(v);
    console_putc('\n');
}

void exception_handler(struct regs *r)
{
    if (r->vector == 0x80) { syscall_dispatch(r); return; }
    if (r->vector >= 32 && r->vector < 48) { irq_dispatch(r->vector - 32); return; }
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    if ((r->cs & 3) == 3) {                 /* fallo en Ring 3: matar solo al proceso */
        console_puts("\n[kernel] proceso de usuario muerto: ");
        console_puts(r->vector < 32 ? names[r->vector] : "Unknown");
        console_puts("\n  RIP="); console_hex(r->rip);
        console_puts(" CR2="); console_hex(cr2);
        console_puts(" ERR="); console_hex(r->error);
        console_putc(0x0A);
        user_cleanup();
        task_exit();
    }

    console_set_color(15, 4);
    console_puts("\n*** NETHEL EXCEPTION ***");
    console_set_color(7, 0);
    console_puts("\n");

    console_puts(r->vector < 32 ? names[r->vector] : "Unknown");
    console_puts("  vector="); console_dec(r->vector);
    console_putc('\n');

    row("ERR    ", r->error);
    row("RIP    ", r->rip);
    row("CR2    ", cr2);
    row("RSP    ", r->rsp);
    row("RFLAGS ", r->rflags);
    row("RAX    ", r->rax);
    row("RBX    ", r->rbx);
    row("RCX    ", r->rcx);
    row("RDX    ", r->rdx);

    console_puts("\nSistema detenido.\n");
    for (;;) __asm__ volatile("cli; hlt");
}
