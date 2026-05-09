#ifndef QOS_RANDOM_H
#define QOS_RANDOM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deterministic userspace PRNG helpers (not cryptographically secure). */
void qos_srand_u64(uint64_t seed);
uint64_t qos_rand_u64(void);
uint32_t qos_rand_u32(void);

#ifdef __cplusplus
}
#endif

#endif
