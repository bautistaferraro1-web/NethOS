#ifndef NETHEL_USER_H
#define NETHEL_USER_H

int  user_spawn(void);               /* programa en assembly pegado al kernel */
int  user_spawn_crash(void);         /* programa que provoca un #PF en Ring 3 */
int  user_spawn_name(const char *name);   /* programa de la tabla; -1 si no existe */
/* Vuelve al espacio del kernel y libera el del proceso (la llama exit) */
void user_cleanup(void);
/* 1 si hay procesos de usuario vivos: el shell del kernel no lee el teclado */
int  user_foreground(void);

void user_kill_self(void);   /* termina la tarea actual como un exit forzado */

#define USER_MAXARGS 8
#define USER_ARGBUF  256
/* argv empaquetado: argc strings con NUL, una detras de otra */
struct uargs { int argc; char buf[USER_ARGBUF]; };
int  user_spawn_args(const char *name, const struct uargs *args);   /* -1 si no existe */

#endif
