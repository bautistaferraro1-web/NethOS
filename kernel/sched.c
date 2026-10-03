#include "sched.h"
#include "heap.h"
#include "cpu.h"
#include "console.h"
#ifndef HOST_TEST
#include "gdt.h"
#include "vmm.h"
#endif

#define STACK_SIZE   16384
#define SLICE_TICKS  10                 /* 100 ms a 100 Hz */

enum { T_READY, T_RUNNING, T_DEAD };

struct task {
    uint32_t id;
    int      state;
    char     name[16];
    uint64_t rsp;
    uint8_t *stack;                     /* 0 para la tarea de arranque */
    uint64_t ticks;
    uint64_t cr3;                       /* espacio de direcciones */
    struct task *next;                  /* lista circular */
};

extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void task_trampoline(void);

static struct task *current;
static uint32_t next_id = 1;
static int slice;

static void copy_name(char *dst, const char *src)
{
    int i = 0;
    while (src[i] && i < 15) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void sched_init(void)
{
    struct task *t = kmalloc(sizeof(*t));
    if (!t) { console_puts("sched: sin memoria\n"); return; }
    t->id = 0;
    t->state = T_RUNNING;
    copy_name(t->name, "kernel");
    t->rsp = 0;
    t->stack = 0;
    t->ticks = 0;
#ifndef HOST_TEST
    t->cr3 = vmm_kernel_space();
#endif
    t->next = t;
    current = t;
}

/* Libera las tareas muertas (menos la actual, que aun usa su stack) */
static void reap(void)
{
    struct task *p = current;
    while (p->next != current) {
        struct task *d = p->next;
        if (d->state == T_DEAD) {
            p->next = d->next;
            kfree(d->stack);
            kfree(d);
            continue;
        }
        p = d;
    }
}

/* Debe llamarse con interrupciones desactivadas */
static void schedule(void)
{
    if (!current) return;
    reap();

    struct task *prev = current;
    struct task *n = prev->next;
    while (n != prev && n->state == T_DEAD) n = n->next;
    if (n == prev) return;                      /* no hay otra tarea */

    if (prev->state == T_RUNNING) prev->state = T_READY;
    n->state = T_RUNNING;
    current = n;
#ifndef HOST_TEST
    if (n->stack) gdt_set_kernel_stack(((uint64_t)n->stack + STACK_SIZE) & ~0xFULL);
    vmm_switch(n->cr3);
#endif
    switch_context(&prev->rsp, n->rsp);
}

int task_create(const char *name, task_fn fn, void *arg)
{
    if (!current) return -1;

    struct task *t = kmalloc(sizeof(*t));
    uint8_t *stack = kmalloc(STACK_SIZE);
    if (!t || !stack) { kfree(t); kfree(stack); return -1; }

    /* Stack inicial: lo que switch_context va a "restaurar" la primera vez */
    uint64_t *sp = (uint64_t *)(stack + STACK_SIZE);
    sp -= 1; sp[0] = (uint64_t)task_trampoline;     /* direccion de retorno */
    sp -= 6;
    sp[0] = 0;                                      /* r15 */
    sp[1] = 0;                                      /* r14 */
    sp[2] = 0;                                      /* r13 */
    sp[3] = (uint64_t)arg;                          /* r12 */
    sp[4] = 0;                                      /* rbp */
    sp[5] = (uint64_t)fn;                           /* rbx */

    copy_name(t->name, name);
    t->state = T_READY;
    t->rsp = (uint64_t)sp;
    t->stack = stack;
    t->ticks = 0;
#ifndef HOST_TEST
    t->cr3 = vmm_kernel_space();
#endif

    uint64_t f = irq_save();
    t->id = next_id++;
    t->next = current->next;
    current->next = t;
    irq_restore(f);
    return (int)t->id;
}

void yield(void)
{
    uint64_t f = irq_save();
    schedule();
    irq_restore(f);
}

void task_exit(void)
{
    (void)irq_save();                           /* no se restaura: esta tarea no vuelve */
    current->state = T_DEAD;
    schedule();
    for (;;) __asm__ volatile("hlt");
}

void sched_tick(void)
{
    if (!current) return;
    current->ticks++;
    if (++slice >= SLICE_TICKS) {
        slice = 0;
        schedule();
    }
}

int sched_task_count(void)
{
    if (!current) return 0;
    uint64_t f = irq_save();
    int n = 0;
    struct task *t = current;
    do { if (t->state != T_DEAD) n++; t = t->next; } while (t != current);
    irq_restore(f);
    return n;
}

void sched_list(void)
{
    uint64_t f = irq_save();
    console_puts("ID  ESTADO    TICKS  NOMBRE\n");
    for (uint32_t id = 0; id < next_id; id++) {
        struct task *t = current;
        do {
            if (t->id == id && t->state != T_DEAD) {
                console_dec(t->id);
                console_puts(t->id < 10 ? "   " : "  ");
                console_puts(t->state == T_RUNNING ? "RUNNING   " : "READY     ");
                console_dec(t->ticks);
                console_puts("  ");
                console_puts(t->name);
                console_putc('\n');
            }
            t = t->next;
        } while (t != current);
    }
    irq_restore(f);
}

#ifndef HOST_TEST
/* Cambia el espacio de direcciones de la tarea actual (y lo activa) */
void task_set_space(uint64_t space)
{
    uint64_t f = irq_save();
    current->cr3 = space;
    vmm_switch(space);
    irq_restore(f);
}

uint64_t task_get_space(void) { return current->cr3; }
#endif

int task_alive(int id)
{
    if (!current) return 0;
    uint64_t f = irq_save();
    int alive = 0;
    struct task *t = current;
    do {
        if ((int)t->id == id && t->state != T_DEAD) { alive = 1; break; }
        t = t->next;
    } while (t != current);
    irq_restore(f);
    return alive;
}
