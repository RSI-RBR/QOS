#ifndef QOS_STDLIB_H
#define QOS_STDLIB_H

#include "qos_user_heap.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RAND_MAX
#define RAND_MAX 2147483647
#endif

void exit(int status);
void srand(unsigned int seed);
int rand(void);

#ifdef __cplusplus
}
#endif

#endif
