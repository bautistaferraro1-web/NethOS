#ifndef NETHEL_KEYBOARD_H
#define NETHEL_KEYBOARD_H

void keyboard_init(void);
int  keyboard_getc(void);     /* -1 si no hay tecla */

void keyboard_set_intr(void (*fn)(void)); /* hook de Ctrl+C */

#endif
