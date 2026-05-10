#include "qos_math.h"

uint64_t qos_isqrt_u64(uint64_t x){
    uint64_t res = 0u;
    uint64_t bit = 1ULL << 62;

    while (bit > x){
        bit >>= 2;
    }

    while (bit != 0u){
        if (x >= res + bit){
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }

    return res;
}

int64_t qos_isqrt_i64(int64_t x){
    if (x < 0){
        return -1;
    }
    return (int64_t)qos_isqrt_u64((uint64_t)x);
}

double sqrt(double x){
    if (x < 0.0){
        return 0.0;
    }
    if (x == 0.0){
        return 0.0;
    }

    union {
        double d;
        uint64_t u;
    } v;

    v.d = x;
    unsigned int exp = (unsigned int)((v.u >> 52) & 0x7FFu);
    if (exp == 0x7FFu){
        return x;
    }

    /*
     * Division-free sqrt for userspace. The old Newton loop used x / guess;
     * on the game build that made faults land in sqrt when DMA/timer pressure
     * was high. This keeps the hot path to multiplies/adds using the classic
     * inverse-sqrt seed plus Newton refinement.
     */
    v.u = 0x5FE6EB50C7B537A9ULL - (v.u >> 1);
    double y = v.d;
    double half = x * 0.5;
    y = y * (1.5 - half * y * y);
    y = y * (1.5 - half * y * y);
    y = y * (1.5 - half * y * y);
    return x * y;
}
