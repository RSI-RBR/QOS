#ifndef PQCLEAN_RANDOMBYTES_H
#define PQCLEAN_RANDOMBYTES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define randombytes PQCLEAN_randombytes
int randombytes(uint8_t *output, size_t n);

#ifdef __cplusplus
}
#endif

#endif
