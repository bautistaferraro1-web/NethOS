#include "user.h"
#include "gdt.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu.h"
#include "sched.h"
#include "console.h"
#include "elf.h"
#include "progs.h"

#define USER_CODE  0x0000008000000000ULL
#define USER_STACK 0x0000008000100000ULL   /* base de la pagina; el tope es +4096 */

/* which: 0 = asm, 1 = crash, 2+i = programa i de la tabla */
enum { PROG_ASM, PROG_CRASH, PROG_TABLE };

extern const uint8_t uprog_start[], uprog_end[];
extern const uint8_t ucrash_start[], ucrash_end[];

static volatile int fg;                    /* procesos de usuario vivos */

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

static int load_elf(uint64_t sp, const uint8_t *s, const uint8_t *e, uint64_t *entry)
{
    return elf_load(s, (uint64_t)(e - s), sp, USER_CODE, USER_STACK, entry);
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
    if (which >= PROG_TABLE) {
        const struct prog *p = prog_at(which - PROG_TABLE);
        rc = p ? load_elf(sp, p->start, p->end, entry) : -1;
    } else if (which == PROG_CRASH) {
        rc = load_blob(sp, ucrash_start, ucrash_end, entry);
    } else {
        rc = load_blob(sp, uprog_start, uprog_end, entry);
    }

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

/* La llaman exit y el handler de excepciones, siempre con IF=0 */
void user_cleanup(void)
{
    uint64_t sp = task_get_space();
    task_set_space(vmm_kernel_space());
    if (sp != vmm_kernel_space()) vmm_destroy_space(sp);
    if (fg > 0) fg--;
}

int user_foreground(void) { return fg > 0; }

static void user_task(void *arg)
{
    int which = (int)(uint64_t)arg;
    uint64_t entry = 0;

    uint64_t f = irq_save();
    int rc = user_setup(which, &entry);
    if (rc && fg > 0) fg--;                   /* no llego a ser proceso */
    irq_restore(f);
    if (rc) return;

    /* rsp0 ya lo dejo puesto schedule() al entrar a esta tarea */
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

/* fg se sube al crear la tarea, asi el shell del kernel no se queda con teclas del proceso */
static int spawn(const char *name, int which)
{
    uint64_t f = irq_save();
    fg++;
    irq_restore(f);

    int id = task_create(name, user_task, (void *)(uint64_t)which);
    if (id < 0) {
        f = irq_save();
        fg--;
        irq_restore(f);
    }
    return id;
}

int user_spawn(void)       { return spawn("user",  PROG_ASM); }
int user_spawn_crash(void) { return spawn("crash", PROG_CRASH); }

int user_spawn_name(const char *name)
{
    int i = prog_find(name);
    if (i < 0) return -1;
    return spawn(prog_at(i)->name, PROG_TABLE + i);
}
