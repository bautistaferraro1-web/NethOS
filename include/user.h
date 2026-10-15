#ifndef NETHEL_USER_H
#define NETHEL_USER_H
#include <stdint.h>

int  user_spawn(void);               /* programa en assembly pegado al kernel */
int  user_spawn_crash(void);         /* programa que provoca un #PF en Ring 3 */
int  user_spawn_name(const char *name);   /* programa de la tabla; -1 si no existe */
/* Vuelve al espacio del kernel y libera el del proceso (la llama exit) */
void user_cleanup(void);
/* 1 si hay procesos de usuario vivos: el shell del kernel no lee el teclado */
int  user_foreground(void);

void user_kill_self(void);   /* termina la tarea actual como un exit forzado */

#define USER_MAXARGS 32
#define USER_MAXENV  32
#define USER_ARGBUF  1024
#define USER_ENVBUF  512
/* argv empaquetado: argc strings con NUL, una detras de otra */
struct uargs { int argc; char buf[USER_ARGBUF]; };
struct uenv  { int envc; char buf[USER_ENVBUF]; };   /* strings "K=V" separadas por \0 */
int  user_spawn_args(const char *name, const struct uargs *args);   /* -1 si no existe */

int  user_wait(int pid, int *status, int nohang);   /* pid hijo, 0 (WNOHANG) o -10 (ECHILD) */
int  user_ppid(void);

long user_clone_test(void);   /* temporal */

struct regs;
int  user_fork(const struct regs *r);   /* id del hijo o -1 */

/* 0 ok (r apunta al programa nuevo); -1 falla sin tocar nada */
int  user_exec(const uint8_t *img, uint64_t size, const struct uargs *a, const struct uenv *env, struct regs *r);
#endif
