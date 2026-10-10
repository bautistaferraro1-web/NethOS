#ifndef INITRAMFS_H
#define INITRAMFS_H
#include "file.h"
/* Devuelve un struct file nuevo (refcount 1) o 0 si no existe */
struct file *initramfs_open(const char *path);
#endif
