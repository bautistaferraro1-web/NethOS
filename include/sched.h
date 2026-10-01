#ifndef NETHEL_SCHED_H
#define NETHEL_SCHED_H
#include <stdint.h>

typedef void (*task_fn)(void *);

void sched_init(void);                                     /* el flujo actual pasa a ser la tarea 0 */
int  task_create(const char *name, task_fn fn, void *arg); /* devuelve el id, o -1 */
void yield(void);                                          /* ceder la CPU voluntariamente */
void task_exit(void);                                      /* terminar la tarea actual */
void sched_tick(void);                                     /* lo llama el timer */
void sched_list(void);                                     /* imprime las tareas */
int  sched_task_count(void);                               /* tareas vivas */

#endif
