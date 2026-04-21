#ifndef LOADER_H
#define LOADER_H

void* load_program(const void *src, unsigned long size, void *dst);

void* load_program_from_sd(const char *path);

#endif
