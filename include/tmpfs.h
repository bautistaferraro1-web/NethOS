#ifndef TMPFS_H
#define TMPFS_H
#include "file.h"

/* Archivo del tmpfs (crea si hay O_CREAT). 0 y *err = errno positivo si falla */
struct file *tmpfs_open(const char *path, int flags, int *err);
struct file *tmpfs_open_as(const char *path, int flags,
                            uint32_t mode, uint32_t uid, uint32_t gid,
                            uint32_t mask, int *err);
int tmpfs_exists(const char *path);
int tmpfs_chmod(const char *path, uint32_t mode, uint32_t euid);
int tmpfs_chown(const char *path, uint32_t uid, uint32_t gid,
                uint32_t euid);
int tmpfs_fchmod(struct file *f, uint32_t mode, uint32_t euid);
int tmpfs_fchown(struct file *f, uint32_t uid, uint32_t gid,
                 uint32_t euid);
int tmpfs_mkdir_as(const char *path, uint32_t mode,
                   uint32_t uid, uint32_t gid, uint32_t mask);
int tmpfs_rmdir(const char *path, uint32_t euid);
int tmpfs_unlink(const char *path, uint32_t euid);   /* 0, -2 no existe, -16 abierto */
#endif
