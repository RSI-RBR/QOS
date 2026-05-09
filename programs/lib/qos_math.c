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
    double guess;

    if (x < 0.0){
        return 0.0;
    }
    if (x == 0.0){
        return 0.0;
    }

    guess = (x >= 1.0) ? x : 1.0;
    for (int i = 0; i < 24; i++){
        guess = 0.5 * (guess + (x / guess));
    }
    return guess;
}
