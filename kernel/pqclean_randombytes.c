#include "randombytes.h"
#include "crypto.h"

int PQCLEAN_randombytes(uint8_t *output, size_t n){
    if (!output){
        return -1;
    }
    if (n == 0u){
        return 0;
    }
    if (n > 0xFFFFFFFFu){
        return -1;
    }
    return crypto_random_bytes(output, (unsigned int)n);
}
