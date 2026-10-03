#include "user.h"
#include "gdt.h"
#include "vmm.h"
#include "pmm.h"
#include "sched.h"
#include "console.h"

#define USER_CODE  0x0000008000000000ULL   /* fuera de la identidad de 4 GiB */
#define USER_STACK 0x0000008000100000ULL   /* base de la pagina; el tope es +4096 */

extern const uint8_t uprog_start[], uprog_end[];

static void zero_page(uint64_t phys)
{
    volatile uint64_t *p = (volatile uint64_t *)phys;
    for (int i = 0; i < 512; i++) p[i] = 0;
}

static void user_task(void *arg)
{
    (void)arg;

    uint64_t code  = pmm_alloc_page();
    uint64_t stack = pmm_alloc_page();
    if (!code || !stack) { console_puts("user: sin memoria\n"); return; }

    uint64_t size = (uint64_t)(uprog_end - uprog_start);
    if (size > 4096) { console_puts("user: programa muy grande\n"); return; }

    zero_page(code);
    zero_page(stack);
    for (uint64_t i = 0; i < size; i++)
        ((volatile uint8_t *)code)[i] = uprog_start[i];   /* identidad: escribo por la fisica */

    if (vmm_map(USER_CODE, code, VMM_USER) ||
        vmm_map(USER_STACK, stack, VMM_USER | VMM_WRITE)) {
        console_puts("user: vmm_map fallo\n");
        return;
    }

    uint64_t rsp0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp0));
    gdt_set_kernel_stack(rsp0 & ~0xFULL);

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

int user_spawn(void) { return task_create("user", user_task, 0); }
