#ifndef INITRAMFS_H
#define INITRAMFS_H
#include "file.h"
/* Devuelve un struct file nuevo (refcount 1) o 0 si no existe */
struct file *initramfs_open(const char *path);
/* 1 = directorio, 0 = archivo, -1 = no existe */
int initramfs_isdir(const char *path);
int initramfs_lookup(const char *path, const uint8_t **data, uint64_t *size);
#endif
