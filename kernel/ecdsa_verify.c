#include "ecdsa_verify.h"
#include "sha256.h"
#include "sha512.h"
#include "string.h"
#include "crypto.h"

#include <stdint.h>
#include <stddef.h>

#define TLS_SIG_ECDSA_SECP256R1_SHA256 0x0403u
#define TLS_SIG_ECDSA_SECP384R1_SHA384 0x0503u
#define TLS_SIG_ECDSA_SECP521R1_SHA512 0x0603u

typedef struct {
    unsigned char tag;
    const unsigned char* val;
    unsigned int val_len;
    unsigned int total_len;
} asn1_tlv_t;

typedef struct {
    uint32_t x[8];
    uint32_t y[8];
} eccp256_point_t;

typedef struct {
    uint32_t x[12];
    uint32_t y[12];
} eccp384_point_t;

int eccp256_ecdsa_verify(eccp256_point_t* p_publicKey,
                         uint32_t p_hash[8],
                         uint32_t r[8],
                         uint32_t s[8]);
void eccp256_bytes2native(uint32_t p_native[8], uint8_t p_bytes[32]);

int eccp384_ecdsa_verify(eccp384_point_t* p_publicKey,
                         uint32_t p_hash[12],
                         uint32_t r[12],
                         uint32_t s[12]);
void eccp384_bytes2native(uint32_t p_native[12], uint8_t p_bytes[48]);

static int asn1_parse_tlv(const unsigned char* p, unsigned int len, asn1_tlv_t* out){
    if (!p || !out || len < 2u){
        return -1;
    }
    unsigned int off = 0u;
    unsigned char tag = p[off++];
    unsigned char lb = p[off++];
    unsigned int vlen = 0u;

    if ((lb & 0x80u) == 0u){
        vlen = (unsigned int)lb;
    } else{
        unsigned int n = (unsigned int)(lb & 0x7Fu);
        if (n == 0u || n > 4u || (off + n) > len){
            return -1;
        }
        for (unsigned int i = 0; i < n; i++){
            vlen = (vlen << 8) | p[off++];
        }
        if (vlen < 128u){
            return -1;
        }
    }
    if ((off + vlen) > len){
        return -1;
    }
    out->tag = tag;
    out->val = p + off;
    out->val_len = vlen;
    out->total_len = off + vlen;
    return 0;
}

static int asn1_integer_positive_bytes(const asn1_tlv_t* i,
                                       const unsigned char** out,
                                       unsigned int* out_len){
    const unsigned char* p;
    unsigned int n;
    if (!i || !out || !out_len || i->tag != 0x02u || i->val_len == 0u){
        return -1;
    }
    if (i->val[0] & 0x80u){
        return -1;
    }
    p = i->val;
    n = i->val_len;
    if (n > 1u && p[0] == 0x00u){
        p++;
        n--;
    }
    if (n == 0u){
        return -1;
    }
    *out = p;
    *out_len = n;
    return 0;
}

static int parse_ecdsa_der_signature(const unsigned char* der_sig,
                                     unsigned int der_sig_len,
                                     unsigned char* out_r,
                                     unsigned char* out_s,
                                     unsigned int coord_len){
    asn1_tlv_t seq;
    asn1_tlv_t rint;
    asn1_tlv_t sint;
    const unsigned char* rptr;
    const unsigned char* sptr;
    unsigned int rlen;
    unsigned int slen;
    unsigned int off = 0u;

    if (!der_sig || der_sig_len == 0u || !out_r || !out_s || coord_len == 0u){
        return -1;
    }

    if (asn1_parse_tlv(der_sig, der_sig_len, &seq) != 0 || seq.tag != 0x30u || seq.total_len != der_sig_len){
        return -1;
    }
    if (asn1_parse_tlv(seq.val + off, seq.val_len - off, &rint) != 0){
        return -1;
    }
    off += rint.total_len;
    if (asn1_parse_tlv(seq.val + off, seq.val_len - off, &sint) != 0){
        return -1;
    }
    off += sint.total_len;
    if (off != seq.val_len){
        return -1;
    }
    if (asn1_integer_positive_bytes(&rint, &rptr, &rlen) != 0 ||
        asn1_integer_positive_bytes(&sint, &sptr, &slen) != 0){
        return -1;
    }
    if (rlen > coord_len || slen > coord_len){
        return -1;
    }
    for (unsigned int i = 0; i < coord_len; i++){
        out_r[i] = 0u;
        out_s[i] = 0u;
    }
    for (unsigned int i = 0; i < rlen; i++){
        out_r[coord_len - rlen + i] = rptr[i];
    }
    for (unsigned int i = 0; i < slen; i++){
        out_s[coord_len - slen + i] = sptr[i];
    }
    return 0;
}

static void sha384_digest(const unsigned char* msg, unsigned int msg_len, unsigned char out[48]){
    sha512_context ctx;
    unsigned char full[64];
    sha512_init(&ctx);
    sha512_update(&ctx, msg, (size_t)msg_len);
    sha512_final(&ctx, full);
    for (unsigned int i = 0; i < 48u; i++){
        out[i] = full[i];
    }
    crypto_memzero(full, sizeof(full));
}

static int compute_hash(unsigned short sig_alg,
                        const unsigned char* message,
                        unsigned int message_len,
                        unsigned char* out_hash,
                        unsigned int* out_hash_len){
    if (!message || !out_hash || !out_hash_len){
        return -1;
    }
    if (sig_alg == TLS_SIG_ECDSA_SECP256R1_SHA256){
        sha256_digest(message, message_len, out_hash);
        *out_hash_len = 32u;
        return 0;
    }
    if (sig_alg == TLS_SIG_ECDSA_SECP384R1_SHA384){
        sha384_digest(message, message_len, out_hash);
        *out_hash_len = 48u;
        return 0;
    }
    if (sig_alg == TLS_SIG_ECDSA_SECP521R1_SHA512){
        if (sha512(message, (size_t)message_len, out_hash) != 0){
            return -1;
        }
        *out_hash_len = 64u;
        return 0;
    }
    return -1;
}

int ecdsa_verify_signature(unsigned int curve_id,
                           const unsigned char* pub_x,
                           const unsigned char* pub_y,
                           unsigned int pub_len,
                           unsigned short sig_alg,
                           const unsigned char* message,
                           unsigned int message_len,
                           const unsigned char* der_signature,
                           unsigned int der_signature_len){
    unsigned char hash[64];
    unsigned int hash_len = 0u;
    unsigned char r_bytes[48];
    unsigned char s_bytes[48];

    if (!pub_x || !pub_y || !message || !der_signature || pub_len == 0u){
        return -1;
    }
    if (compute_hash(sig_alg, message, message_len, hash, &hash_len) != 0){
        return -1;
    }

    if (curve_id == ECDSA_VERIFY_CURVE_P256){
        uint8_t qx[32];
        uint8_t qy[32];
        uint8_t h_in[32];
        uint32_t qx_n[8];
        uint32_t qy_n[8];
        uint32_t h_n[8];
        uint32_t r_n[8];
        uint32_t s_n[8];
        eccp256_point_t pub;

        if (pub_len != 32u || parse_ecdsa_der_signature(der_signature, der_signature_len, r_bytes, s_bytes, 32u) != 0){
            return -1;
        }
        for (unsigned int i = 0; i < 32u; i++){
            qx[i] = pub_x[i];
            qy[i] = pub_y[i];
            h_in[i] = 0u;
        }
        if (hash_len >= 32u){
            for (unsigned int i = 0; i < 32u; i++){
                h_in[i] = hash[i];
            }
        } else{
            for (unsigned int i = 0; i < hash_len; i++){
                h_in[32u - hash_len + i] = hash[i];
            }
        }

        eccp256_bytes2native(qx_n, qx);
        eccp256_bytes2native(qy_n, qy);
        eccp256_bytes2native(h_n, h_in);
        eccp256_bytes2native(r_n, r_bytes);
        eccp256_bytes2native(s_n, s_bytes);

        for (unsigned int i = 0; i < 8u; i++){
            pub.x[i] = qx_n[i];
            pub.y[i] = qy_n[i];
        }
        crypto_memzero(qx, sizeof(qx));
        crypto_memzero(qy, sizeof(qy));
        crypto_memzero(h_in, sizeof(h_in));
        crypto_memzero(qx_n, sizeof(qx_n));
        crypto_memzero(qy_n, sizeof(qy_n));

        if (!eccp256_ecdsa_verify(&pub, h_n, r_n, s_n)){
            crypto_memzero(h_n, sizeof(h_n));
            crypto_memzero(r_n, sizeof(r_n));
            crypto_memzero(s_n, sizeof(s_n));
            crypto_memzero(&pub, sizeof(pub));
            return -1;
        }
        crypto_memzero(h_n, sizeof(h_n));
        crypto_memzero(r_n, sizeof(r_n));
        crypto_memzero(s_n, sizeof(s_n));
        crypto_memzero(&pub, sizeof(pub));
        return 0;
    }

    if (curve_id == ECDSA_VERIFY_CURVE_P384){
        uint8_t qx[48];
        uint8_t qy[48];
        uint8_t h_in[48];
        uint32_t qx_n[12];
        uint32_t qy_n[12];
        uint32_t h_n[12];
        uint32_t r_n[12];
        uint32_t s_n[12];
        eccp384_point_t pub;

        if (pub_len != 48u || parse_ecdsa_der_signature(der_signature, der_signature_len, r_bytes, s_bytes, 48u) != 0){
            return -1;
        }
        for (unsigned int i = 0; i < 48u; i++){
            qx[i] = pub_x[i];
            qy[i] = pub_y[i];
            h_in[i] = 0u;
        }
        if (hash_len >= 48u){
            for (unsigned int i = 0; i < 48u; i++){
                h_in[i] = hash[i];
            }
        } else{
            for (unsigned int i = 0; i < hash_len; i++){
                h_in[48u - hash_len + i] = hash[i];
            }
        }

        eccp384_bytes2native(qx_n, qx);
        eccp384_bytes2native(qy_n, qy);
        eccp384_bytes2native(h_n, h_in);
        eccp384_bytes2native(r_n, r_bytes);
        eccp384_bytes2native(s_n, s_bytes);

        for (unsigned int i = 0; i < 12u; i++){
            pub.x[i] = qx_n[i];
            pub.y[i] = qy_n[i];
        }
        crypto_memzero(qx, sizeof(qx));
        crypto_memzero(qy, sizeof(qy));
        crypto_memzero(h_in, sizeof(h_in));
        crypto_memzero(qx_n, sizeof(qx_n));
        crypto_memzero(qy_n, sizeof(qy_n));

        if (!eccp384_ecdsa_verify(&pub, h_n, r_n, s_n)){
            crypto_memzero(h_n, sizeof(h_n));
            crypto_memzero(r_n, sizeof(r_n));
            crypto_memzero(s_n, sizeof(s_n));
            crypto_memzero(&pub, sizeof(pub));
            return -1;
        }
        crypto_memzero(h_n, sizeof(h_n));
        crypto_memzero(r_n, sizeof(r_n));
        crypto_memzero(s_n, sizeof(s_n));
        crypto_memzero(&pub, sizeof(pub));
        return 0;
    }

    return -1;
}
