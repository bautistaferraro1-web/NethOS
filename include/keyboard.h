#ifndef NETHEL_KEYBOARD_H
#define NETHEL_KEYBOARD_H

void keyboard_init(void);
int  keyboard_getc(void);     /* -1 si no hay tecla */
int  keyboard_has_data(void);

void keyboard_set_susp(void (*fn)(void)); /* hook de Ctrl+Z */
void keyboard_set_intr(void (*fn)(void)); /* hook de Ctrl+C */

#endif
