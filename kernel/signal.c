#include "sig.h"
#include "proc.h"
#include "sched.h"
#include "idt.h"
#include "user.h"

int user_range_ok(uint64_t p, uint64_t n);

struct sigframe {
    uint64_t pretcode;          /* direccion de retorno = restorer */
    struct regs saved;          /* contexto interrumpido */
    uint64_t oldmask;
    uint8_t  siginfo[128];
};

int sig_default_ignored(int sig)
{
    switch (sig) {
    case SIGCHLD: case SIGURG: case SIGWINCH: case SIGCONT:
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return 1;
    }
    return 0;
}

void sig_send(struct proc *p, int sig)
{
    if (sig < 1 || sig >= NSIG) return;
    if (sig != SIGKILL && sig != SIGSTOP) {
        uint64_t h = p->sa[sig - 1].handler;
        if (h == SIG_IGN) return;
        if (h == SIG_DFL && sig_default_ignored(sig)) return;
    }
    p->sig_pending |= SIG_BIT(sig);
}

void sig_reset_on_exec(struct proc *p)
{
    for (int i = 0; i < NSIG; i++)
        if (p->sa[i].handler != SIG_IGN) {
            p->sa[i].handler = SIG_DFL; p->sa[i].flags = 0;
            p->sa[i].restorer = 0;      p->sa[i].mask = 0;
        }
}

static void copy_words(void *d, const void *s, uint64_t n)
{
    uint64_t *dd = d; const uint64_t *ss = s;
    for (uint64_t i = 0; i < n / 8; i++) dd[i] = ss[i];
}

void sig_deliver(struct regs *r, int from_syscall, uint64_t nr)
{
    if ((r->cs & 3) != 3) return;
    struct proc *p = task_proc();

    uint64_t ready = p->sig_pending & (~p->sig_blocked | SIG_UNBLOCKABLE);
    while (ready) {
        int sig = __builtin_ctzll(ready) + 1;
        p->sig_pending &= ~SIG_BIT(sig);
        ready = p->sig_pending & (~p->sig_blocked | SIG_UNBLOCKABLE);

        struct ksigaction *sa = &p->sa[sig - 1];
        if (sig == SIGKILL || (sa->handler == SIG_DFL && !sig_default_ignored(sig)))
            user_kill_sig(sig);
        if (sa->handler == SIG_DFL || sa->handler == SIG_IGN) continue;

        /* syscall interrumpida: reiniciar si SA_RESTART (int 0x80 y syscall miden 2 bytes) */
        if (from_syscall && r->rax == (uint64_t)-4 && (sa->flags & SA_RESTART)) {
            r->rip -= 2;
            r->rax = nr;
        }

        uint64_t sp = ((r->rsp - 128 - sizeof(struct sigframe)) & ~15ULL) - 8;
        if (!user_range_ok(sp, sizeof(struct sigframe))) user_kill_sig(SIGSEGV);
        struct sigframe *f = (struct sigframe *)sp;

        f->pretcode = (sa->flags & SA_RESTORER) ? sa->restorer : 0;
        copy_words(&f->saved, r, sizeof *r);
        f->oldmask = p->sig_blocked;
        for (int i = 0; i < 128; i++) f->siginfo[i] = 0;
        *(int *)f->siginfo = sig;

        p->sig_blocked |= sa->mask;
        if (!(sa->flags & SA_NODEFER)) p->sig_blocked |= SIG_BIT(sig);
        p->sig_blocked &= ~SIG_UNBLOCKABLE;
        if (sa->flags & SA_RESETHAND) sa->handler = SIG_DFL;

        r->rip = sa->handler;
        r->rsp = sp;
        r->rdi = (uint64_t)sig;
        r->rsi = (uint64_t)f->siginfo;
        r->rdx = (uint64_t)&f->saved;
        r->rax = 0;
        r->rflags &= ~0x400ULL;
        return;
    }
}

void sig_sigreturn(struct regs *r)
{
    uint64_t sp = r->rsp - 8;
    if (!user_range_ok(sp, sizeof(struct sigframe))) user_kill_sig(SIGSEGV);
    struct sigframe *f = (struct sigframe *)sp;
    struct proc *p = task_proc();

    struct regs s;
    copy_words(&s, &f->saved, sizeof s);
    if (s.rip >= 0x0000800000000000ULL || s.rsp >= 0x0000800000000000ULL)
        user_kill_sig(SIGSEGV);
    s.cs = 0x23; s.ss = 0x1B;
    s.rflags = (s.rflags & 0xCD5ULL) | 0x202;
    p->sig_blocked = f->oldmask & ~SIG_UNBLOCKABLE;
    copy_words(r, &s, sizeof s);
}
