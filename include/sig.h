#ifndef SIG_H
#define SIG_H
#include <stdint.h>

struct regs;
struct proc;

#define NSIG 64
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGWINCH 28

#define SIG_DFL 0ULL
#define SIG_IGN 1ULL

#define SA_SIGINFO   0x00000004ULL
#define SA_RESTORER  0x04000000ULL
#define SA_RESTART   0x10000000ULL
#define SA_NODEFER   0x40000000ULL
#define SA_RESETHAND 0x80000000ULL

#define SIG_BIT(s)      (1ULL << ((s) - 1))
#define SIG_UNBLOCKABLE (SIG_BIT(SIGKILL) | SIG_BIT(SIGSTOP))

/* Mismo layout que kernel_sigaction de x86_64 (32 bytes) */
struct ksigaction { uint64_t handler, flags, restorer, mask; };

void sig_send(struct proc *p, int sig);
void sig_deliver(struct regs *r, int from_syscall, uint64_t nr);
void sig_sigreturn(struct regs *r);
void sig_reset_on_exec(struct proc *p);
int  sig_default_ignored(int sig);
#endif
