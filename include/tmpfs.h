#ifndef TMPFS_H
#define TMPFS_H
#include "file.h"

/* Archivo del tmpfs (crea si hay O_CREAT). 0 y *err = errno positivo si falla */
struct file *tmpfs_open(const char *path, int flags, int *err);
int tmpfs_exists(const char *path);
int tmpfs_unlink(const char *path);   /* 0, -2 no existe, -16 abierto */
#endif
