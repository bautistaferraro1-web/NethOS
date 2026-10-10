#include "user.h"
#include "gdt.h"
#include "vmm.h"
#include "pmm.h"
#include "mem.h"
#include "cpu.h"
#include "sched.h"
#include "console.h"
#include "elf.h"
#include "progs.h"
#include "keyboard.h"
#include "heap.h"
#include "proc.h"
#include "file.h"

#define USER_CODE        0x0000000000400000ULL   /* donde enlazan los ELF estandar */
#define USER_STACK_PAGES 16
#define USER_STACK_TOP   0x00007FFFFFFFF000ULL   /* fin (exclusivo); la pagina siguiente queda sin mapear */
#define USER_STACK       (USER_STACK_TOP - USER_STACK_PAGES * 4096ULL)   /* base */

/* which: 0 = asm, 1 = crash, 2+i = programa i de la tabla */
enum { PROG_ASM, PROG_CRASH, PROG_TABLE };

extern const uint8_t uprog_start[], uprog_end[];
extern const uint8_t ucrash_start[], ucrash_end[];

static volatile int fg;                    /* procesos de usuario vivos */

/* ---- tabla de procesos: padre, estado y status de salida (para wait4) ---- */
#define PT_MAX 64
enum { PT_FREE, PT_RUN, PT_ZOMBIE };
struct pent { int id, ppid, state, status; };
static struct pent ptab[PT_MAX];

static struct pent *pt_find(int id)
{
    for (int i = 0; i < PT_MAX; i++)
        if (ptab[i].state && ptab[i].id == id) return &ptab[i];
    return 0;
}

static int pt_add(int id, int ppid)
{
    for (int i = 0; i < PT_MAX; i++)
        if (ptab[i].state == PT_FREE) {
            ptab[i].id = id; ptab[i].ppid = ppid; ptab[i].state = PT_RUN; ptab[i].status = 0;
            return 0;
        }
    return -1;
}

static void pt_exit(int id, int status)
{
    uint64_t f = irq_save();
    for (int i = 0; i < PT_MAX; i++)                    /* hijos propios: huerfanos */
        if (ptab[i].state && ptab[i].ppid == id) {
            if (ptab[i].state == PT_ZOMBIE) ptab[i].state = PT_FREE;
            else ptab[i].ppid = 0;
        }
    struct pent *e = pt_find(id);
    if (e) {
        if (e->ppid == 0) e->state = PT_FREE;           /* nadie lo va a esperar */
        else { e->state = PT_ZOMBIE; e->status = status; }
    }
    irq_restore(f);
}

int user_ppid(void)
{
    uint64_t f = irq_save();
    struct pent *e = pt_find(task_id());
    int p = e ? e->ppid : 0;
    irq_restore(f);
    return p;
}

int user_wait(int pid, int *status, int nohang)
{
    int me = task_id();
    for (;;) {
        uint64_t f = irq_save();
        int have = 0;
        for (int i = 0; i < PT_MAX; i++) {
            struct pent *e = &ptab[i];
            if (!e->state || e->ppid != me) continue;
            if (pid > 0 && e->id != pid) continue;
            have = 1;
            if (e->state == PT_ZOMBIE) {
                int id = e->id;
                if (status) *status = e->status;
                e->state = PT_FREE;
                irq_restore(f);
                return id;
            }
        }
        irq_restore(f);
        if (!have) return -10;                          /* ECHILD */
        if (nohang) return 0;
        if (task_killed()) user_kill_self();
        yield();
        __asm__ volatile("sti; hlt; cli" : : : "memory");
    }
}

static void zero_page(uint64_t phys)
{
    volatile uint64_t *p = (volatile uint64_t *)P2V(phys);
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
    for (uint64_t i = 0; i < size; i++) ((volatile uint8_t *)P2V(code))[i] = start[i];

    if (vmm_map_in(sp, USER_CODE, code, VMM_USER)) {
        pmm_free_page(code);
        console_puts("user: vmm_map fallo\n");
        return -1;
    }
    *entry = USER_CODE;
    return 0;
}

static int load_elf(uint64_t sp, const uint8_t *s, const uint8_t *e, uint64_t *entry, struct elf_aux *ai)
{
    return elf_load(s, (uint64_t)(e - s), sp, USER_CODE, USER_STACK, entry, ai);
}

/* Lo que spawn() le pasa a la tarea nueva (se libera en user_task) */
struct launch { int which; struct uargs args; };

/* 16 bytes "aleatorios" para AT_RANDOM (TSC + xorshift: suficiente por ahora) */
static void fill_random(uint8_t *dst)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = (((uint64_t)hi << 32) | lo) ^ 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 16; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        dst[i] = (uint8_t)(x >> 24);
    }
}

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_ENTRY  9
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define NAUX      9

/* Arma el stack inicial System V en 'frame', la pagina fisica de ARRIBA del stack
   (mapeada en USER_STACK_TOP - 4096). Devuelve el rsp inicial, alineado a 16:
     rsp -> argc | argv[0..argc-1] | NULL | envp: NULL | auxv (pares) ... | random(16) | strings */
static uint64_t build_stack(uint64_t frame, const struct uargs *a, uint64_t entry, const struct elf_aux *ai)
{
    const uint64_t page = USER_STACK_TOP - 4096;
    uint64_t used = 0;
    for (int i = 0; i < a->argc; i++) { while (a->buf[used]) used++; used++; }

    uint64_t strva = (USER_STACK_TOP - used) & ~0xFULL;
    uint64_t rndva = strva - 16;
    uint64_t nw    = 1 + (uint64_t)a->argc + 1 + 1 + NAUX * 2;
    uint64_t sp    = (rndva - nw * 8) & ~0xFULL;

    volatile uint8_t *pg = (volatile uint8_t *)P2V(frame);
    for (uint64_t i = 0; i < used; i++) pg[strva - page + i] = (uint8_t)a->buf[i];
    uint8_t rnd[16];
    fill_random(rnd);
    for (int i = 0; i < 16; i++) pg[rndva - page + i] = rnd[i];

    volatile uint64_t *w = (volatile uint64_t *)P2V(frame + (sp - page));
    uint64_t k = 0, off = 0;
    w[k++] = (uint64_t)a->argc;
    for (int i = 0; i < a->argc; i++) {
        w[k++] = strva + off;
        while (a->buf[off]) off++;
        off++;
    }
    w[k++] = 0;                              /* argv[argc] = NULL */
    w[k++] = 0;                              /* envp[0]    = NULL */
#define AUX(t, v) do { w[k++] = (t); w[k++] = (v); } while (0)
    AUX(AT_PAGESZ, 4096);
    AUX(AT_CLKTCK, 100);
    AUX(AT_PHDR,   ai->phdr);
    AUX(AT_PHENT,  ai->phent);
    AUX(AT_PHNUM,  ai->phnum);
    AUX(AT_ENTRY,  entry);
    AUX(AT_RANDOM, rndva);
    AUX(AT_SECURE, 0);
    AUX(AT_NULL,   0);
#undef AUX
    return sp;
}

/* Se llama con interrupciones desactivadas (el PMM no es seguro con preemption) */
static int user_setup(int which, const struct uargs *args, uint64_t *entry, uint64_t *rsp)
{
    struct elf_aux ai = { 0, 0, 0, 0 };
    uint64_t sp = vmm_create_space();
    if (!sp) { console_puts("user: sin memoria\n"); return -1; }

    int rc;
    if (which >= PROG_TABLE) {
        const struct prog *p = prog_at(which - PROG_TABLE);
        rc = p ? load_elf(sp, p->start, p->end, entry, &ai) : -1;
    } else if (which == PROG_CRASH) {
        rc = load_blob(sp, ucrash_start, ucrash_end, entry);
    } else {
        rc = load_blob(sp, uprog_start, uprog_end, entry);
    }

    uint64_t top_frame = 0;
    for (int i = 0; !rc && i < USER_STACK_PAGES; i++) {
        uint64_t f = pmm_alloc_page();
        if (!f) { console_puts("user: sin memoria para el stack\n"); rc = -1; break; }
        zero_page(f);
        if (vmm_map_in(sp, USER_STACK + (uint64_t)i * 4096, f, VMM_USER | VMM_WRITE)) {
            pmm_free_page(f);                 /* aun no es del espacio */
            console_puts("user: vmm_map del stack fallo\n");
            rc = -1;
            break;
        }
        if (i == USER_STACK_PAGES - 1) top_frame = f;
    }
    if (rc) {
        vmm_destroy_space(sp);                /* libera lo ya mapeado */
        return -1;
    }

    *rsp = build_stack(top_frame, args, *entry, &ai);
    {
        struct proc *pr = task_proc();
        uint64_t end = which >= PROG_TABLE ? ai.end : USER_CODE + 4096;
        pr->brk_start = pr->brk_cur = (end + 0xFFFULL) & ~0xFFFULL;
        pr->mmap_next = USER_MMAP_BASE;
        fd_init_std(pr->fds);
        pr->cwd[0] = '/'; pr->cwd[1] = 0;
        pr->exit_code = 11;                   /* si muere sin exit: SIGSEGV */
    }
    task_set_space(sp);                       /* a partir de aca CR3 = espacio del proceso */
    return 0;
}

/* La llaman exit y el handler de excepciones, siempre con IF=0 */
void user_cleanup(void)
{
    struct proc *pr = task_proc();
    if (pr) fd_close_all(pr->fds);        /* libera archivos abiertos en cualquier tipo de muerte */
    pt_exit(task_id(), pr ? pr->exit_code : 11);
    uint64_t sp = task_get_space();
    task_set_space(vmm_kernel_space());
    if (sp != vmm_kernel_space()) vmm_destroy_space(sp);
    if (fg > 0) fg--;
}

int user_foreground(void) { return fg > 0; }

/* La tarea actual muere por Ctrl+C: mismo camino que exit */
void user_kill_self(void)
{
    (void)irq_save();                     /* no se restaura: no vuelve */
    console_puts("\n[kernel] proceso terminado (Ctrl+C)\n");
    task_proc()->exit_code = 2;           /* SIGINT */
    user_cleanup();
    task_exit();
}

static void user_task(void *arg)
{
    struct launch *l = arg;
    int which = l->which;
    uint64_t rsp = 0;
    uint64_t entry = 0;

    task_mark_user();

    uint64_t f = irq_save();
    int rc = user_setup(which, &l->args, &entry, &rsp);
    kfree(l);
    if (rc && fg > 0) fg--;                   /* no llego a ser proceso */
    irq_restore(f);
    if (rc) { pt_exit(task_id(), 127 << 8); return; }

    /* rsp0 ya lo dejo puesto schedule() al entrar a esta tarea */
    __asm__ volatile(
        "cli\n\t"
        "pushq %0\n\t"          /* SS     */
        "pushq %1\n\t"          /* RSP    */
        "pushq $0x202\n\t"      /* RFLAGS (IF=1) */
        "pushq %2\n\t"          /* CS     */
        "pushq %3\n\t"          /* RIP    */
        /* ABI de Linux: el proceso arranca con los registros generales en 0
           (glibc toma rdx como rtld_fini; ademas evita filtrar punteros del kernel) */
        "xorl %%eax, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "xorl %%edx, %%edx\n\t"
        "xorl %%esi, %%esi\n\t"
        "xorl %%edi, %%edi\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "xorl %%r8d, %%r8d\n\t"
        "xorl %%r9d, %%r9d\n\t"
        "xorl %%r10d, %%r10d\n\t"
        "xorl %%r11d, %%r11d\n\t"
        "xorl %%r12d, %%r12d\n\t"
        "xorl %%r13d, %%r13d\n\t"
        "xorl %%r14d, %%r14d\n\t"
        "xorl %%r15d, %%r15d\n\t"
        "iretq\n\t"
        : : "i"(SEL_UDATA), "r"(rsp),
            "i"(SEL_UCODE), "r"(entry) : "memory");

    for (;;) __asm__ volatile("hlt");
}

/* fg se sube al crear la tarea, asi el shell del kernel no se queda con teclas del proceso */
static int spawn(const char *name, int which, const struct uargs *args)
{
    keyboard_set_intr(task_interrupt_user);

    uint64_t f = irq_save();
    struct launch *l = kmalloc(sizeof(*l));
    if (l) fg++;
    irq_restore(f);
    if (!l) return -1;

    l->which = which;
    l->args.argc = args ? args->argc : 0;
    for (int i = 0; i < USER_ARGBUF; i++) l->args.buf[i] = args ? args->buf[i] : 0;

    uint64_t g = irq_save();
    int id = task_create(name, user_task, l);
    if (id >= 0) pt_add(id, task_id());
    irq_restore(g);
    if (id < 0) {
        f = irq_save();
        fg--;
        kfree(l);
        irq_restore(f);
    }
    return id;
}

int user_spawn(void)       { return spawn("user",  PROG_ASM,   0); }
int user_spawn_crash(void) { return spawn("crash", PROG_CRASH, 0); }

int user_spawn_args(const char *name, const struct uargs *args)
{
    int i = prog_find(name);
    if (i < 0) return -1;
    return spawn(prog_at(i)->name, PROG_TABLE + i, args);
}

int user_spawn_name(const char *name)           /* desde el shell del kernel: argv = [name] */
{
    struct uargs a;
    int n = 0;
    while (name[n] && n < 15) { a.buf[n] = name[n]; n++; }
    a.buf[n] = 0;
    a.argc = 1;
    return user_spawn_args(name, &a);
}
