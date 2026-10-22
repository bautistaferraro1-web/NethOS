#ifndef NETHEL_FAT32_H
#define NETHEL_FAT32_H

#include "file.h"

void fat32_init(void);
struct file *fat32_open(const char *path);
int fat32_handles_path(const char *path);

#endif
