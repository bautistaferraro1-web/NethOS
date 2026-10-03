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
extern const uint8_t ucrash_start[], ucrash_end[];

static void zero_page(uint64_t phys)
{
    volatile uint64_t *p = (volatile uint64_t *)phys;
    for (int i = 0; i < 512; i++) p[i] = 0;
}

/* Libera lo que haya quedado sin dueño. 'code'/'stack' van en 0 si ya los posee el espacio. */
static void fail(const char *msg, uint64_t sp, uint64_t code, uint64_t stack)
{
    console_puts(msg);
    if (code)  pmm_free_page(code);
    if (stack) pmm_free_page(stack);
    if (sp)    vmm_destroy_space(sp);
}

/* Se llama con interrupciones desactivadas (el PMM no es seguro con preemption) */
static int user_setup(int which)
{
    const uint8_t *start = which ? ucrash_start : uprog_start;
    const uint8_t *end   = which ? ucrash_end   : uprog_end;
    uint64_t size = (uint64_t)(end - start);
    if (size > 4096) { console_puts("user: programa muy grande\n"); return -1; }

    uint64_t sp    = vmm_create_space();
    uint64_t code  = pmm_alloc_page();
    uint64_t stack = pmm_alloc_page();
    if (!sp || !code || !stack) { fail("user: sin memoria\n", sp, code, stack); return -1; }

    zero_page(code);
    zero_page(stack);
    for (uint64_t i = 0; i < size; i++)
        ((volatile uint8_t *)code)[i] = start[i];   /* identidad: escribo por la fisica */

    if (vmm_map_in(sp, USER_CODE, code, VMM_USER)) {
        fail("user: vmm_map fallo\n", sp, code, stack);
        return -1;
    }
    if (vmm_map_in(sp, USER_STACK, stack, VMM_USER | VMM_WRITE)) {
        fail("user: vmm_map fallo\n", sp, 0, stack);      /* 'code' ya es del espacio */
        return -1;
    }

    task_set_space(sp);                                   /* a partir de aca CR3 = espacio del proceso */
    return 0;
}

/* La llama exit: vuelve al espacio del kernel y libera el del proceso */
void user_cleanup(void)
{
    uint64_t sp = task_get_space();
    task_set_space(vmm_kernel_space());
    if (sp != vmm_kernel_space()) vmm_destroy_space(sp);
}

static void user_task(void *arg)
{


    uint64_t f = irq_save();
    int rc = user_setup((int)(uint64_t)arg);
    irq_restore(f);
    if (rc) return;

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

int user_spawn(void) { return task_create("user", user_task, 0); }
int user_spawn_crash(void) { return task_create("crash", user_task, (void *)1); }
