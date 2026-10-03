#include "user.h"
#include "gdt.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu.h"
#include "sched.h"
#include "console.h"

#define USER_CODE  0x0000008000000000ULL   /* fuera de la identidad de 4 GiB */
#define USER_STACK 0x0000008000100000ULL   /* base de la pagina; el tope es +4096 */

extern const uint8_t uprog_start[], uprog_end[];

static uint64_t code_phys, stack_phys;
static volatile int active;

static void zero_page(uint64_t phys)
{
    volatile uint64_t *p = (volatile uint64_t *)phys;
    for (int i = 0; i < 512; i++) p[i] = 0;
}

void user_cleanup(void)
{
    if (code_phys)  { vmm_unmap(USER_CODE);  pmm_free_page(code_phys);  code_phys = 0; }
    if (stack_phys) { vmm_unmap(USER_STACK); pmm_free_page(stack_phys); stack_phys = 0; }
    active = 0;
}

static void user_task(void *arg)
{
    (void)arg;

    code_phys  = pmm_alloc_page();
    stack_phys = pmm_alloc_page();
    if (!code_phys || !stack_phys) {
        console_puts("user: sin memoria\n");
        user_cleanup();
        return;
    }

    uint64_t size = (uint64_t)(uprog_end - uprog_start);
    if (size > 4096) {
        console_puts("user: programa muy grande\n");
        user_cleanup();
        return;
    }

    zero_page(code_phys);
    zero_page(stack_phys);
    for (uint64_t i = 0; i < size; i++)
        ((volatile uint8_t *)code_phys)[i] = uprog_start[i];  /* identidad: escribo por la fisica */

    if (vmm_map(USER_CODE, code_phys, VMM_USER) ||
        vmm_map(USER_STACK, stack_phys, VMM_USER | VMM_WRITE)) {
        console_puts("user: vmm_map fallo\n");
        user_cleanup();
        return;
    }

    /* rsp0 ya lo dejo puesto schedule() al entrar a esta tarea */
    console_puts("user: entrando a ring 3\n");

    __asm__ volatile(
        "cli\n\t"
        "pushq %0\n\t"          /* SS     */
        "pushq %1\n\t"          /* RSP    */
        "pushq $0x202\n\t"      /* RFLAGS (IF=1) */
        "pushq %2\n\t"          /* CS     */
        "pushq %3\n\t"          /* RIP    */
        "iretq\n\t"
        : : "i"(SEL_UDATA), "r"(USER_STACK + 4096),
            "i"(SEL_UCODE), "r"(USER_CODE) : "memory");

    for (;;) __asm__ volatile("hlt");
}

int user_spawn(void)
{
    uint64_t f = irq_save();
    if (active) {
        irq_restore(f);
        console_puts("user: ya hay un proceso de usuario\n");
        return -1;
    }
    active = 1;
    irq_restore(f);

    int id = task_create("user", user_task, 0);
    if (id < 0) active = 0;
    return id;
}
