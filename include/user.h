#ifndef NETHEL_USER_H
#define NETHEL_USER_H

/* Crea una tarea que entra a Ring 3. Devuelve el id o -1 */
int  user_spawn(void);
int  user_spawn_crash(void);
/* Desmapea y libera las paginas del proceso de usuario (la llama exit) */
void user_cleanup(void);

#endif
