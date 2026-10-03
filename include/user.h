#ifndef NETHEL_USER_H
#define NETHEL_USER_H

int  user_spawn(void);          /* programa en assembly pegado al kernel */
int  user_spawn_crash(void);    /* programa que provoca un #PF en Ring 3 */
int  user_spawn_elf(void);      /* hello.elf, cargado con el cargador ELF */
/* Vuelve al espacio del kernel y libera el del proceso (la llama exit) */
void user_cleanup(void);

#endif
