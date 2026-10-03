#ifndef NETHEL_USER_H
#define NETHEL_USER_H

int  user_spawn(void);          /* programa en assembly pegado al kernel */
int  user_spawn_crash(void);    /* programa que provoca un #PF en Ring 3 */
int  user_spawn_elf(void);      /* hello.elf */
int  user_spawn_echo(void);     /* echo.elf (usa read) */
/* Vuelve al espacio del kernel y libera el del proceso (la llama exit) */
void user_cleanup(void);
/* 1 si hay procesos de usuario vivos: el shell del kernel no lee el teclado */
int  user_foreground(void);

#endif
