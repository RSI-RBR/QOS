#include "qos_random.h"
#include <stdlib.h>

static uint64_t g_rng_state = 0x243F6A8885A308D3ULL;

static uint64_t seed_mix64(uint64_t x){
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

void qos_srand_u64(uint64_t seed){
    uint64_t mixed = seed_mix64(seed);
    if (mixed == 0ULL){
        mixed = 0x9E3779B97F4A7C15ULL;
    }
    g_rng_state = mixed;
}

uint64_t qos_rand_u64(void){
    uint64_t x = g_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

uint32_t qos_rand_u32(void){
    return (uint32_t)(qos_rand_u64() >> 32);
}

void srand(unsigned int seed){
    qos_srand_u64(((uint64_t)seed << 1) | 1ULL);
}

int rand(void){
    return (int)(qos_rand_u32() & (uint32_t)RAND_MAX);
}
