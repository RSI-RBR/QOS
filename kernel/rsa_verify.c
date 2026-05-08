#include "rsa_verify.h"
#include "sha256.h"
#include "sha512.h"

#include <stddef.h>

#define TLS_SIG_RSA_PKCS1_SHA256 0x0401u
#define TLS_SIG_RSA_PKCS1_SHA384 0x0501u
#define TLS_SIG_RSA_PKCS1_SHA512 0x0601u
#define TLS_SIG_RSA_PSS_RSAE_SHA256 0x0804u
#define TLS_SIG_RSA_PSS_RSAE_SHA384 0x0805u
#define TLS_SIG_RSA_PSS_RSAE_SHA512 0x0806u
#define TLS_SIG_RSA_PSS_PSS_SHA256 0x0809u
#define TLS_SIG_RSA_PSS_PSS_SHA384 0x080Au
#define TLS_SIG_RSA_PSS_PSS_SHA512 0x080Bu

static void mem_zero(unsigned char* p, unsigned int n) {
    for (unsigned int i = 0; i < n; ++i) {
        p[i] = 0;
    }
}

static int mem_eq(const unsigned char* a, const unsigned char* b, unsigned int n) {
    unsigned char diff = 0;
    for (unsigned int i = 0; i < n; ++i) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static int bn_cmp(const unsigned char* a, const unsigned char* b, unsigned int n) {
    for (unsigned int i = 0; i < n; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

static void bn_sub_inplace(unsigned char* a, const unsigned char* b, unsigned int n) {
    unsigned int borrow = 0;
    for (int i = (int)n - 1; i >= 0; --i) {
        unsigned int av = a[i];
        unsigned int bv = b[i] + borrow;
        if (av < bv) {
            a[i] = (unsigned char)(av + 256u - bv);
            borrow = 1;
        } else {
            a[i] = (unsigned char)(av - bv);
            borrow = 0;
        }
    }
}

static void bn_mod_double(unsigned char* a, const unsigned char* mod, unsigned int n) {
    unsigned int carry = 0;
    for (int i = (int)n - 1; i >= 0; --i) {
        unsigned int v = ((unsigned int)a[i] << 1) | carry;
        a[i] = (unsigned char)(v & 0xFFu);
        carry = (v >> 8) & 1u;
    }
    if (carry || bn_cmp(a, mod, n) >= 0) {
        bn_sub_inplace(a, mod, n);
    }
}

static void bn_mod_add(unsigned char* a, const unsigned char* b, const unsigned char* mod, unsigned int n) {
    unsigned int carry = 0;
    for (int i = (int)n - 1; i >= 0; --i) {
        unsigned int v = (unsigned int)a[i] + b[i] + carry;
        a[i] = (unsigned char)(v & 0xFFu);
        carry = (v >> 8) & 1u;
    }
    if (carry || bn_cmp(a, mod, n) >= 0) {
        bn_sub_inplace(a, mod, n);
    }
}

static void bn_mod_mul(unsigned char* out,
                       const unsigned char* x,
                       const unsigned char* y,
                       const unsigned char* mod,
                       unsigned int n) {
    unsigned char r[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned char a[RSA_VERIFY_MAX_MOD_BYTES];

    mem_zero(r, n);
    for (unsigned int i = 0; i < n; ++i) {
        a[i] = x[i];
    }
    if (bn_cmp(a, mod, n) >= 0) {
        bn_sub_inplace(a, mod, n);
    }

    for (unsigned int byte = 0; byte < n; ++byte) {
        unsigned char v = y[byte];
        for (int bit = 7; bit >= 0; --bit) {
            bn_mod_double(r, mod, n);
            if ((v >> bit) & 1u) {
                bn_mod_add(r, a, mod, n);
            }
        }
    }

    for (unsigned int i = 0; i < n; ++i) {
        out[i] = r[i];
    }
}

static int exp_first_bit(const unsigned char* exp, unsigned int exp_len, unsigned int* out_byte, int* out_bit) {
    for (unsigned int i = 0; i < exp_len; ++i) {
        if (exp[i] == 0) continue;
        for (int bit = 7; bit >= 0; --bit) {
            if ((exp[i] >> bit) & 1u) {
                *out_byte = i;
                *out_bit = bit;
                return 0;
            }
        }
    }
    return -1;
}

static int rsa_modexp(unsigned char* out,
                      const unsigned char* sig,
                      unsigned int sig_len,
                      const unsigned char* modulus,
                      unsigned int modulus_len,
                      const unsigned char* exponent,
                      unsigned int exponent_len) {
    unsigned char base[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned char result[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned char tmp[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned int first_byte = 0;
    int first_bit = 0;

    if (!out || !sig || !modulus || !exponent) return -1;
    if (modulus_len == 0 || modulus_len > RSA_VERIFY_MAX_MOD_BYTES) return -1;
    if (exponent_len == 0 || exponent_len > RSA_VERIFY_MAX_EXP_BYTES) return -1;
    if (sig_len == 0 || sig_len > modulus_len) return -1;

    mem_zero(base, modulus_len);
    for (unsigned int i = 0; i < sig_len; ++i) {
        base[modulus_len - sig_len + i] = sig[i];
    }
    if (bn_cmp(base, modulus, modulus_len) >= 0) {
        return -1;
    }

    mem_zero(result, modulus_len);
    result[modulus_len - 1] = 1;

    if (exp_first_bit(exponent, exponent_len, &first_byte, &first_bit) != 0) {
        return -1;
    }

    for (unsigned int byte = first_byte; byte < exponent_len; ++byte) {
        int start_bit = (byte == first_byte) ? first_bit : 7;
        for (int bit = start_bit; bit >= 0; --bit) {
            bn_mod_mul(tmp, result, result, modulus, modulus_len);
            for (unsigned int i = 0; i < modulus_len; ++i) result[i] = tmp[i];

            if ((exponent[byte] >> bit) & 1u) {
                bn_mod_mul(tmp, result, base, modulus, modulus_len);
                for (unsigned int i = 0; i < modulus_len; ++i) result[i] = tmp[i];
            }
        }
    }

    for (unsigned int i = 0; i < modulus_len; ++i) {
        out[i] = result[i];
    }
    return 0;
}

static const unsigned char sha256_digestinfo_prefix[] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};

static const unsigned char sha384_digestinfo_prefix[] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30
};

static const unsigned char sha512_digestinfo_prefix[] = {
    0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40
};

static void sha384_digest(const unsigned char* msg, unsigned int msg_len, unsigned char out[48]) {
    sha512_context ctx;
    unsigned char full[64];

    sha512_init(&ctx);
    ctx.state[0] = UINT64_C(0xcbbb9d5dc1059ed8);
    ctx.state[1] = UINT64_C(0x629a292a367cd507);
    ctx.state[2] = UINT64_C(0x9159015a3070dd17);
    ctx.state[3] = UINT64_C(0x152fecd8f70e5939);
    ctx.state[4] = UINT64_C(0x67332667ffc00b31);
    ctx.state[5] = UINT64_C(0x8eb44a8768581511);
    ctx.state[6] = UINT64_C(0xdb0c2e0d64f98fa7);
    ctx.state[7] = UINT64_C(0x47b5481dbefa4fa4);
    sha512_update(&ctx, msg, (size_t)msg_len);
    sha512_final(&ctx, full);
    for (unsigned int i = 0; i < 48u; i++) {
        out[i] = full[i];
    }
}

static int hash_bytes_for_sig_alg(unsigned short sig_alg,
                                  const unsigned char* msg,
                                  unsigned int msg_len,
                                  unsigned char* digest,
                                  unsigned int* digest_len) {
    if (sig_alg == TLS_SIG_RSA_PKCS1_SHA256 ||
        sig_alg == TLS_SIG_RSA_PSS_RSAE_SHA256 ||
        sig_alg == TLS_SIG_RSA_PSS_PSS_SHA256) {
        sha256_digest(msg, msg_len, digest);
        *digest_len = 32;
        return 0;
    }
    if (sig_alg == TLS_SIG_RSA_PKCS1_SHA384 ||
        sig_alg == TLS_SIG_RSA_PSS_RSAE_SHA384 ||
        sig_alg == TLS_SIG_RSA_PSS_PSS_SHA384) {
        sha384_digest(msg, msg_len, digest);
        *digest_len = 48;
        return 0;
    }
    if (sig_alg == TLS_SIG_RSA_PKCS1_SHA512 ||
        sig_alg == TLS_SIG_RSA_PSS_RSAE_SHA512 ||
        sig_alg == TLS_SIG_RSA_PSS_PSS_SHA512) {
        sha512(msg, (size_t)msg_len, digest);
        *digest_len = 64;
        return 0;
    }
    return -1;
}

static int hash_for_sig_alg(unsigned short sig_alg,
                            const unsigned char* msg,
                            unsigned int msg_len,
                            unsigned char* digest,
                            unsigned int* digest_len,
                            const unsigned char** di_prefix,
                            unsigned int* di_prefix_len) {
    if (hash_bytes_for_sig_alg(sig_alg, msg, msg_len, digest, digest_len) != 0) {
        return -1;
    }
    if (sig_alg == TLS_SIG_RSA_PKCS1_SHA256 ||
        sig_alg == TLS_SIG_RSA_PSS_RSAE_SHA256 ||
        sig_alg == TLS_SIG_RSA_PSS_PSS_SHA256) {
        if (di_prefix) *di_prefix = sha256_digestinfo_prefix;
        if (di_prefix_len) *di_prefix_len = sizeof(sha256_digestinfo_prefix);
        return 0;
    }
    if (sig_alg == TLS_SIG_RSA_PKCS1_SHA384 ||
        sig_alg == TLS_SIG_RSA_PSS_RSAE_SHA384 ||
        sig_alg == TLS_SIG_RSA_PSS_PSS_SHA384) {
        if (di_prefix) *di_prefix = sha384_digestinfo_prefix;
        if (di_prefix_len) *di_prefix_len = sizeof(sha384_digestinfo_prefix);
        return 0;
    }
    if (sig_alg == TLS_SIG_RSA_PKCS1_SHA512 ||
        sig_alg == TLS_SIG_RSA_PSS_RSAE_SHA512 ||
        sig_alg == TLS_SIG_RSA_PSS_PSS_SHA512) {
        if (di_prefix) *di_prefix = sha512_digestinfo_prefix;
        if (di_prefix_len) *di_prefix_len = sizeof(sha512_digestinfo_prefix);
        return 0;
    }
    return -1;
}

static int verify_pkcs1_v15(const unsigned char* em,
                            unsigned int em_len,
                            const unsigned char* digest,
                            unsigned int digest_len,
                            const unsigned char* di_prefix,
                            unsigned int di_prefix_len) {
    unsigned int i;

    if (em_len < 3u + 8u + di_prefix_len + digest_len) return -1;
    if (em[0] != 0x00u || em[1] != 0x01u) return -1;

    i = 2;
    while (i < em_len && em[i] == 0xFFu) {
        ++i;
    }
    if (i < 10u) return -1;
    if (i >= em_len || em[i] != 0x00u) return -1;
    ++i;

    if (i + di_prefix_len + digest_len != em_len) return -1;
    if (!mem_eq(em + i, di_prefix, di_prefix_len)) return -1;
    i += di_prefix_len;
    if (!mem_eq(em + i, digest, digest_len)) return -1;

    return 0;
}

static int mgf1_for_sig_alg(unsigned short sig_alg,
                            const unsigned char* seed,
                            unsigned int seed_len,
                            unsigned char* out,
                            unsigned int out_len) {
    unsigned int produced = 0;
    unsigned int counter = 0;
    unsigned char block[68];
    unsigned char digest[64];
    unsigned int digest_len = 0;

    if (!seed || !out || seed_len > 64u) return -1;
    for (unsigned int i = 0; i < seed_len; ++i) {
        block[i] = seed[i];
    }

    while (produced < out_len) {
        block[seed_len + 0] = (unsigned char)((counter >> 24) & 0xFFu);
        block[seed_len + 1] = (unsigned char)((counter >> 16) & 0xFFu);
        block[seed_len + 2] = (unsigned char)((counter >> 8) & 0xFFu);
        block[seed_len + 3] = (unsigned char)(counter & 0xFFu);
        if (hash_bytes_for_sig_alg(sig_alg, block, seed_len + 4u, digest, &digest_len) != 0){
            return -1;
        }
        for (unsigned int i = 0; i < digest_len && produced < out_len; ++i) {
            out[produced++] = digest[i];
        }
        ++counter;
    }
    return 0;
}

static unsigned int bn_bitlen(const unsigned char* n, unsigned int n_len) {
    for (unsigned int i = 0; i < n_len; ++i) {
        if (n[i] == 0) continue;
        unsigned int bits = (n_len - i - 1u) * 8u;
        unsigned char v = n[i];
        for (int b = 7; b >= 0; --b) {
            if ((v >> b) & 1u) {
                return bits + (unsigned int)b + 1u;
            }
        }
    }
    return 0;
}

static int verify_pss(const unsigned char* em,
                      unsigned int em_full_len,
                      unsigned int mod_bits,
                      unsigned short sig_alg,
                      const unsigned char* msg_hash,
                      unsigned int hash_len) {
    unsigned char db[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned char mask[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned char h2[64];
    unsigned char mprime[136];
    unsigned int em_bits;
    unsigned int em_len;
    unsigned int leading;
    unsigned int db_len;
    unsigned int ps_len;

    if ((hash_len != 32u && hash_len != 48u && hash_len != 64u) || mod_bits == 0) return -1;
    em_bits = mod_bits - 1u;
    em_len = (em_bits + 7u) / 8u;
    if (em_len == 0 || em_len > em_full_len || em_len > RSA_VERIFY_MAX_MOD_BYTES) return -1;
    em = em + (em_full_len - em_len);

    if (em_len < hash_len + hash_len + 2u) return -1;
    if (em[em_len - 1u] != 0xBCu) return -1;

    db_len = em_len - hash_len - 1u;
    leading = 8u * em_len - em_bits;
    if (leading && (em[0] & (unsigned char)(0xFFu << (8u - leading))) != 0) return -1;

    if (mgf1_for_sig_alg(sig_alg, em + db_len, hash_len, mask, db_len) != 0) return -1;
    for (unsigned int i = 0; i < db_len; ++i) {
        db[i] = (unsigned char)(em[i] ^ mask[i]);
    }
    if (leading) {
        db[0] &= (unsigned char)(0xFFu >> leading);
    }

    ps_len = db_len - hash_len - 1u;
    for (unsigned int i = 0; i < ps_len; ++i) {
        if (db[i] != 0) return -1;
    }
    if (db[ps_len] != 0x01u) return -1;

    for (unsigned int i = 0; i < 8u; ++i) mprime[i] = 0;
    for (unsigned int i = 0; i < hash_len; ++i) mprime[8u + i] = msg_hash[i];
    for (unsigned int i = 0; i < hash_len; ++i) mprime[8u + hash_len + i] = db[ps_len + 1u + i];
    if (hash_bytes_for_sig_alg(sig_alg, mprime, 8u + hash_len + hash_len, h2, &hash_len) != 0){
        return -1;
    }

    return mem_eq(h2, em + db_len, hash_len) ? 0 : -1;
}

int rsa_verify_x509_signature(const unsigned char* modulus,
                              unsigned int modulus_len,
                              const unsigned char* exponent,
                              unsigned int exponent_len,
                              unsigned short sig_alg,
                              const unsigned char* tbs,
                              unsigned int tbs_len,
                              const unsigned char* signature,
                              unsigned int signature_len) {
    unsigned char em[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned char digest[64];
    const unsigned char* di_prefix = 0;
    unsigned int di_prefix_len = 0;
    unsigned int digest_len = 0;

    while (modulus_len > 0 && *modulus == 0) {
        ++modulus;
        --modulus_len;
    }
    while (exponent_len > 0 && *exponent == 0) {
        ++exponent;
        --exponent_len;
    }

    if (modulus_len == 0 || modulus_len > RSA_VERIFY_MAX_MOD_BYTES) return -1;
    if (exponent_len == 0 || exponent_len > RSA_VERIFY_MAX_EXP_BYTES) return -1;
    if (signature_len == 0 || signature_len > modulus_len) return -1;

    if (hash_for_sig_alg(sig_alg, tbs, tbs_len, digest, &digest_len, &di_prefix, &di_prefix_len) != 0) {
        return -1;
    }

    if (rsa_modexp(em, signature, signature_len, modulus, modulus_len, exponent, exponent_len) != 0) {
        return -1;
    }

    if (sig_alg == TLS_SIG_RSA_PKCS1_SHA256 ||
        sig_alg == TLS_SIG_RSA_PKCS1_SHA384 ||
        sig_alg == TLS_SIG_RSA_PKCS1_SHA512) {
        return verify_pkcs1_v15(em, modulus_len, digest, digest_len, di_prefix, di_prefix_len);
    }

    if (sig_alg == TLS_SIG_RSA_PSS_RSAE_SHA256 ||
        sig_alg == TLS_SIG_RSA_PSS_RSAE_SHA384 ||
        sig_alg == TLS_SIG_RSA_PSS_RSAE_SHA512 ||
        sig_alg == TLS_SIG_RSA_PSS_PSS_SHA256 ||
        sig_alg == TLS_SIG_RSA_PSS_PSS_SHA384 ||
        sig_alg == TLS_SIG_RSA_PSS_PSS_SHA512) {
        return verify_pss(em, modulus_len, bn_bitlen(modulus, modulus_len), sig_alg, digest, digest_len);
    }

    return -1;
}
