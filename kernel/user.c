#include "user.h"
#include "gdt.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu.h"
#include "sched.h"
#include "console.h"
#include "elf.h"

#define USER_CODE  0x0000008000000000ULL
#define USER_STACK 0x0000008000100000ULL   /* base de la pagina; el tope es +4096 */

enum { PROG_ASM, PROG_CRASH, PROG_ELF };

extern const uint8_t uprog_start[], uprog_end[];
extern const uint8_t ucrash_start[], ucrash_end[];
extern const uint8_t _binary_hello_elf_start[], _binary_hello_elf_end[];

static void zero_page(uint64_t phys)
{
    volatile uint64_t *p = (volatile uint64_t *)phys;
    for (int i = 0; i < 512; i++) p[i] = 0;
}

/* Programa crudo: una pagina de codigo en USER_CODE */
static int load_blob(uint64_t sp, const uint8_t *start, const uint8_t *end, uint64_t *entry)
{
    uint64_t size = (uint64_t)(end - start);
    if (size > 4096) { console_puts("user: programa muy grande\n"); return -1; }

    uint64_t code = pmm_alloc_page();
    if (!code) { console_puts("user: sin memoria\n"); return -1; }
    zero_page(code);
    for (uint64_t i = 0; i < size; i++) ((volatile uint8_t *)code)[i] = start[i];

    if (vmm_map_in(sp, USER_CODE, code, VMM_USER)) {
        pmm_free_page(code);
        console_puts("user: vmm_map fallo\n");
        return -1;
    }
    *entry = USER_CODE;
    return 0;
}

/* Se llama con interrupciones desactivadas (el PMM no es seguro con preemption) */
static int user_setup(int which, uint64_t *entry)
{
    uint64_t sp    = vmm_create_space();
    uint64_t stack = pmm_alloc_page();
    if (!sp || !stack) {
        console_puts("user: sin memoria\n");
        if (stack) pmm_free_page(stack);
        if (sp)    vmm_destroy_space(sp);
        return -1;
    }
    zero_page(stack);

    int rc;
    if (which == PROG_ELF)
        rc = elf_load(_binary_hello_elf_start,
                      (uint64_t)(_binary_hello_elf_end - _binary_hello_elf_start),
                      sp, USER_CODE, USER_STACK, entry);
    else if (which == PROG_CRASH)
        rc = load_blob(sp, ucrash_start, ucrash_end, entry);
    else
        rc = load_blob(sp, uprog_start, uprog_end, entry);

    if (!rc && vmm_map_in(sp, USER_STACK, stack, VMM_USER | VMM_WRITE)) {
        console_puts("user: vmm_map del stack fallo\n");
        rc = -1;
    }
    if (rc) {
        pmm_free_page(stack);                 /* aun no es del espacio */
        vmm_destroy_space(sp);                /* libera lo ya mapeado */
        return -1;
    }

    task_set_space(sp);                       /* a partir de aca CR3 = espacio del proceso */
    return 0;
}

void user_cleanup(void)
{
    uint64_t sp = task_get_space();
    task_set_space(vmm_kernel_space());
    if (sp != vmm_kernel_space()) vmm_destroy_space(sp);
}

static void user_task(void *arg)
{
    int which = (int)(uint64_t)arg;
    uint64_t entry = 0;

    uint64_t f = irq_save();
    int rc = user_setup(which, &entry);
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
            "i"(SEL_UCODE), "r"(entry) : "memory");

    for (;;) __asm__ volatile("hlt");
}

int user_spawn(void)       { return task_create("user",  user_task, (void *)(uint64_t)PROG_ASM); }
int user_spawn_crash(void) { return task_create("crash", user_task, (void *)(uint64_t)PROG_CRASH); }
int user_spawn_elf(void)   { return task_create("hello", user_task, (void *)(uint64_t)PROG_ELF); }
