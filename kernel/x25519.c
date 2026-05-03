#include "x25519.h"
#include "crypto.h"
#include "fe.h"

static void clamp_scalar(unsigned char e[32]){
    e[0] &= 248u;
    e[31] &= 127u;
    e[31] |= 64u;
}

static int x25519_scalar_mult(unsigned char out[32],
                              const unsigned char scalar[32],
                              const unsigned char u[32]){
    if (!out || !scalar || !u){
        return -1;
    }

    unsigned char e[32];
    for (unsigned int i = 0; i < 32u; i++){
        e[i] = scalar[i];
    }
    clamp_scalar(e);

    fe x1, x2, z2, x3, z3;
    fe a, b, aa, bb, e1;
    fe c, d, da, cb;
    fe t0, t1;

    fe_frombytes(x1, u);
    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);

    unsigned int swap = 0;
    for (int t = 254; t >= 0; t--){
        unsigned int k_t = (unsigned int)((e[t / 8] >> (t & 7)) & 1u);
        swap ^= k_t;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = k_t;

        // Montgomery ladder step (RFC 7748).
        fe_add(a, x2, z2);        // A = x2 + z2
        fe_sub(b, x2, z2);        // B = x2 - z2
        fe_sq(aa, a);             // AA = A^2
        fe_sq(bb, b);             // BB = B^2
        fe_sub(e1, aa, bb);       // E = AA - BB
        fe_add(c, x3, z3);        // C = x3 + z3
        fe_sub(d, x3, z3);        // D = x3 - z3
        fe_mul(da, d, a);         // DA = D*A
        fe_mul(cb, c, b);         // CB = C*B

        fe_add(t0, da, cb);       // DA + CB
        fe_sq(x3, t0);            // x3 = (DA + CB)^2

        fe_sub(t0, da, cb);       // DA - CB
        fe_sq(t0, t0);            // (DA - CB)^2
        fe_mul(z3, x1, t0);       // z3 = x1*(DA - CB)^2

        fe_mul(x2, aa, bb);       // x2 = AA*BB
        fe_mul121666(t0, e1);     // t0 = 121666*E
        fe_sub(t0, t0, e1);       // t0 = 121665*E (a24 for this formula)
        fe_add(t0, t0, aa);       // AA + 121665*E
        fe_mul(z2, e1, t0);       // z2 = E*(AA + 121665*E)
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);

    crypto_memzero(e, sizeof(e));
    return 0;
}

int x25519_public_from_private(const unsigned char private_key[32],
                               unsigned char public_key_out[32]){
    static const unsigned char base_u[32] = {
        9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };
    return x25519_scalar_mult(public_key_out, private_key, base_u);
}

int x25519_shared_secret(const unsigned char private_key[32],
                         const unsigned char peer_public_key[32],
                         unsigned char shared_secret_out[32]){
    if (x25519_scalar_mult(shared_secret_out, private_key, peer_public_key) != 0){
        return -1;
    }
    if (x25519_is_all_zero(shared_secret_out)){
        return -1;
    }
    return 0;
}

int x25519_generate_private(unsigned char private_key_out[32]){
    if (!private_key_out){
        return -1;
    }
    if (crypto_random_bytes(private_key_out, 32u) != 0){
        return -1;
    }
    clamp_scalar(private_key_out);
    return 0;
}

int x25519_generate_keypair(unsigned char private_key_out[32],
                            unsigned char public_key_out[32]){
    if (!private_key_out || !public_key_out){
        return -1;
    }
    if (x25519_generate_private(private_key_out) != 0){
        return -1;
    }
    return x25519_public_from_private(private_key_out, public_key_out);
}

int x25519_is_all_zero(const unsigned char k[32]){
    if (!k){
        return 1;
    }
    unsigned char acc = 0;
    for (unsigned int i = 0; i < 32u; i++){
        acc |= k[i];
    }
    return acc == 0;
}

int x25519_self_test(void){
    static const unsigned char alice_priv[32] = {
        0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,
        0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
        0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,
        0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a
    };
    static const unsigned char alice_pub_expected[32] = {
        0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,
        0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
        0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,
        0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a
    };
    static const unsigned char bob_priv[32] = {
        0x5d,0xab,0x08,0x7e,0x62,0x4a,0x8a,0x4b,
        0x79,0xe1,0x7f,0x8b,0x83,0x80,0x0e,0xe6,
        0x6f,0x3b,0xb1,0x29,0x26,0x18,0xb6,0xfd,
        0x1c,0x2f,0x8b,0x27,0xff,0x88,0xe0,0xeb
    };
    static const unsigned char bob_pub_expected[32] = {
        0xde,0x9e,0xdb,0x7d,0x7b,0x7d,0xc1,0xb4,
        0xd3,0x5b,0x61,0xc2,0xec,0xe4,0x35,0x37,
        0x3f,0x83,0x43,0xc8,0x5b,0x78,0x67,0x4d,
        0xad,0xfc,0x7e,0x14,0x6f,0x88,0x2b,0x4f
    };
    static const unsigned char shared_expected[32] = {
        0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,
        0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,
        0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,
        0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42
    };

    unsigned char alice_pub[32];
    unsigned char bob_pub[32];
    unsigned char shared1[32];
    unsigned char shared2[32];

    if (x25519_public_from_private(alice_priv, alice_pub) != 0){
        return -1;
    }
    if (x25519_public_from_private(bob_priv, bob_pub) != 0){
        return -1;
    }
    if (!crypto_consttime_equal(alice_pub, alice_pub_expected, 32u)){
        return -1;
    }
    if (!crypto_consttime_equal(bob_pub, bob_pub_expected, 32u)){
        return -1;
    }
    if (x25519_shared_secret(alice_priv, bob_pub, shared1) != 0){
        return -1;
    }
    if (x25519_shared_secret(bob_priv, alice_pub, shared2) != 0){
        return -1;
    }
    if (!crypto_consttime_equal(shared1, shared_expected, 32u)){
        return -1;
    }
    if (!crypto_consttime_equal(shared1, shared2, 32u)){
        return -1;
    }
    return 0;
}
