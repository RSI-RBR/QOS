#include "aes_gcm.h"
#include "crypto.h"

static const unsigned char sbox[256] = {
    0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76,
    0xCA,0x82,0xC9,0x7D,0xFA,0x59,0x47,0xF0,0xAD,0xD4,0xA2,0xAF,0x9C,0xA4,0x72,0xC0,
    0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15,
    0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75,
    0x09,0x83,0x2C,0x1A,0x1B,0x6E,0x5A,0xA0,0x52,0x3B,0xD6,0xB3,0x29,0xE3,0x2F,0x84,
    0x53,0xD1,0x00,0xED,0x20,0xFC,0xB1,0x5B,0x6A,0xCB,0xBE,0x39,0x4A,0x4C,0x58,0xCF,
    0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8,
    0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2,
    0xCD,0x0C,0x13,0xEC,0x5F,0x97,0x44,0x17,0xC4,0xA7,0x7E,0x3D,0x64,0x5D,0x19,0x73,
    0x60,0x81,0x4F,0xDC,0x22,0x2A,0x90,0x88,0x46,0xEE,0xB8,0x14,0xDE,0x5E,0x0B,0xDB,
    0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79,
    0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08,
    0xBA,0x78,0x25,0x2E,0x1C,0xA6,0xB4,0xC6,0xE8,0xDD,0x74,0x1F,0x4B,0xBD,0x8B,0x8A,
    0x70,0x3E,0xB5,0x66,0x48,0x03,0xF6,0x0E,0x61,0x35,0x57,0xB9,0x86,0xC1,0x1D,0x9E,
    0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF,
    0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16
};

static const unsigned int rcon[15] = {
    0x01000000u, 0x02000000u, 0x04000000u, 0x08000000u, 0x10000000u,
    0x20000000u, 0x40000000u, 0x80000000u, 0x1B000000u, 0x36000000u,
    0x6C000000u, 0xD8000000u, 0xAB000000u, 0x4D000000u, 0x9A000000u
};

static unsigned int load_be32(const unsigned char in[4]){
    return ((unsigned int)in[0] << 24)
         | ((unsigned int)in[1] << 16)
         | ((unsigned int)in[2] << 8)
         | ((unsigned int)in[3]);
}

static void store_be64(unsigned char out[8], unsigned long long v){
    for (unsigned int i = 0; i < 8u; i++){
        unsigned int shift = (unsigned int)(56u - (8u * i));
        out[i] = (unsigned char)((v >> shift) & 0xFFu);
    }
}

static unsigned int rot_word(unsigned int w){
    return (w << 8) | (w >> 24);
}

static unsigned int sub_word(unsigned int w){
    unsigned char b0 = sbox[(w >> 24) & 0xFFu];
    unsigned char b1 = sbox[(w >> 16) & 0xFFu];
    unsigned char b2 = sbox[(w >> 8) & 0xFFu];
    unsigned char b3 = sbox[w & 0xFFu];
    return ((unsigned int)b0 << 24)
         | ((unsigned int)b1 << 16)
         | ((unsigned int)b2 << 8)
         | ((unsigned int)b3);
}

static unsigned char xtime(unsigned char x){
    return (unsigned char)((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0x00u));
}

static void add_round_key(unsigned char state[16], const unsigned int* rk){
    for (unsigned int c = 0; c < 4u; c++){
        unsigned int w = rk[c];
        state[4u * c + 0u] ^= (unsigned char)((w >> 24) & 0xFFu);
        state[4u * c + 1u] ^= (unsigned char)((w >> 16) & 0xFFu);
        state[4u * c + 2u] ^= (unsigned char)((w >> 8) & 0xFFu);
        state[4u * c + 3u] ^= (unsigned char)(w & 0xFFu);
    }
}

static void sub_bytes(unsigned char state[16]){
    for (unsigned int i = 0; i < 16u; i++){
        state[i] = sbox[state[i]];
    }
}

static void shift_rows(unsigned char state[16]){
    unsigned char t[16];

    t[0] = state[0];   t[1] = state[5];   t[2] = state[10];  t[3] = state[15];
    t[4] = state[4];   t[5] = state[9];   t[6] = state[14];  t[7] = state[3];
    t[8] = state[8];   t[9] = state[13];  t[10] = state[2];  t[11] = state[7];
    t[12] = state[12]; t[13] = state[1];  t[14] = state[6];  t[15] = state[11];

    for (unsigned int i = 0; i < 16u; i++){
        state[i] = t[i];
    }
}

static void mix_columns(unsigned char state[16]){
    for (unsigned int c = 0; c < 4u; c++){
        unsigned int b = 4u * c;
        unsigned char s0 = state[b + 0u];
        unsigned char s1 = state[b + 1u];
        unsigned char s2 = state[b + 2u];
        unsigned char s3 = state[b + 3u];

        unsigned char t = (unsigned char)(s0 ^ s1 ^ s2 ^ s3);
        unsigned char u = s0;

        state[b + 0u] = (unsigned char)(s0 ^ t ^ xtime((unsigned char)(s0 ^ s1)));
        state[b + 1u] = (unsigned char)(s1 ^ t ^ xtime((unsigned char)(s1 ^ s2)));
        state[b + 2u] = (unsigned char)(s2 ^ t ^ xtime((unsigned char)(s2 ^ s3)));
        state[b + 3u] = (unsigned char)(s3 ^ t ^ xtime((unsigned char)(s3 ^ u)));
    }
}

static void aes_encrypt_block(const aes_gcm_key_t* key,
                              const unsigned char in[16],
                              unsigned char out[16]){
    unsigned char state[16];
    unsigned int round = 0;

    for (unsigned int i = 0; i < 16u; i++){
        state[i] = in[i];
    }

    add_round_key(state, &key->rk[0]);

    for (round = 1; round < key->nr; round++){
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, &key->rk[4u * round]);
    }

    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, &key->rk[4u * key->nr]);

    for (unsigned int i = 0; i < 16u; i++){
        out[i] = state[i];
    }
    crypto_memzero(state, sizeof(state));
}

static void xor_block(unsigned char dst[16], const unsigned char src[16]){
    for (unsigned int i = 0; i < 16u; i++){
        dst[i] ^= src[i];
    }
}

static void gcm_shift_right_one(unsigned char v[16]){
    unsigned char carry = 0;
    for (unsigned int i = 0; i < 16u; i++){
        unsigned char next_carry = (unsigned char)(v[i] & 1u);
        v[i] = (unsigned char)((v[i] >> 1) | (carry ? 0x80u : 0x00u));
        carry = next_carry;
    }
}

static void gcm_gf_mul(unsigned char x[16], const unsigned char h[16]){
    unsigned char z[16];
    unsigned char v[16];

    for (unsigned int i = 0; i < 16u; i++){
        z[i] = 0;
        v[i] = h[i];
    }

    for (unsigned int i = 0; i < 128u; i++){
        unsigned char xi = (unsigned char)((x[i / 8u] >> (7u - (i % 8u))) & 1u);
        if (xi){
            for (unsigned int j = 0; j < 16u; j++){
                z[j] ^= v[j];
            }
        }
        unsigned char lsb = (unsigned char)(v[15] & 1u);
        gcm_shift_right_one(v);
        if (lsb){
            v[0] ^= 0xE1u;
        }
    }

    for (unsigned int i = 0; i < 16u; i++){
        x[i] = z[i];
    }
    crypto_memzero(z, sizeof(z));
    crypto_memzero(v, sizeof(v));
}

static void ghash_update(unsigned char y[16],
                         const unsigned char h[16],
                         const unsigned char* data,
                         unsigned int len){
    if (!data || len == 0u){
        return;
    }

    while (len >= 16u){
        for (unsigned int i = 0; i < 16u; i++){
            y[i] ^= data[i];
        }
        gcm_gf_mul(y, h);
        data += 16u;
        len -= 16u;
    }

    if (len){
        unsigned char block[16];
        for (unsigned int i = 0; i < 16u; i++){
            block[i] = 0;
        }
        for (unsigned int i = 0; i < len; i++){
            block[i] = data[i];
        }
        for (unsigned int i = 0; i < 16u; i++){
            y[i] ^= block[i];
        }
        gcm_gf_mul(y, h);
        crypto_memzero(block, sizeof(block));
    }
}

static void ghash_compute(unsigned char out[16],
                          const unsigned char h[16],
                          const unsigned char* aad, unsigned int aad_len,
                          const unsigned char* c, unsigned int c_len){
    unsigned char y[16];
    unsigned char lens[16];

    for (unsigned int i = 0; i < 16u; i++){
        y[i] = 0;
    }

    ghash_update(y, h, aad, aad_len);
    ghash_update(y, h, c, c_len);

    store_be64(&lens[0], (unsigned long long)aad_len * 8ull);
    store_be64(&lens[8], (unsigned long long)c_len * 8ull);
    for (unsigned int i = 0; i < 16u; i++){
        y[i] ^= lens[i];
    }
    gcm_gf_mul(y, h);

    for (unsigned int i = 0; i < 16u; i++){
        out[i] = y[i];
    }
    crypto_memzero(y, sizeof(y));
    crypto_memzero(lens, sizeof(lens));
}

static void inc32(unsigned char counter[16]){
    for (int i = 15; i >= 12; i--){
        counter[i] = (unsigned char)(counter[i] + 1u);
        if (counter[i] != 0u){
            break;
        }
    }
}

static void build_j0(const aes_gcm_key_t* key,
                     const unsigned char* iv, unsigned int iv_len,
                     unsigned char j0[16]){
    if (iv_len == 12u){
        for (unsigned int i = 0; i < 12u; i++){
            j0[i] = iv[i];
        }
        j0[12] = 0;
        j0[13] = 0;
        j0[14] = 0;
        j0[15] = 1;
        return;
    }
    ghash_compute(j0, key->h, 0, 0, iv, iv_len);
}

int aes_gcm_key_init(aes_gcm_key_t* key,
                     const unsigned char* raw_key,
                     unsigned int raw_key_len){
    if (!key || !raw_key){
        return -1;
    }
    if (raw_key_len != 16u && raw_key_len != 32u){
        return -1;
    }

    unsigned int nk = raw_key_len / 4u;
    unsigned int nr = (nk == 4u) ? 10u : 14u;
    unsigned int nwords = 4u * (nr + 1u);

    for (unsigned int i = 0; i < nk; i++){
        key->rk[i] = load_be32(&raw_key[4u * i]);
    }
    for (unsigned int i = nk; i < nwords; i++){
        unsigned int temp = key->rk[i - 1u];
        if ((i % nk) == 0u){
            temp = sub_word(rot_word(temp)) ^ rcon[(i / nk) - 1u];
        } else if (nk > 6u && (i % nk) == 4u){
            temp = sub_word(temp);
        }
        key->rk[i] = key->rk[i - nk] ^ temp;
    }

    key->nr = nr;
    {
        unsigned char zero[16];
        for (unsigned int i = 0; i < 16u; i++){
            zero[i] = 0;
        }
        aes_encrypt_block(key, zero, key->h);
        crypto_memzero(zero, sizeof(zero));
    }
    return 0;
}

int aes_gcm_encrypt(const aes_gcm_key_t* key,
                    const unsigned char* iv, unsigned int iv_len,
                    const unsigned char* aad, unsigned int aad_len,
                    const unsigned char* pt, unsigned int pt_len,
                    unsigned char* ct,
                    unsigned char* tag, unsigned int tag_len){
    if (!key || !iv || !tag){
        return -1;
    }
    if (tag_len == 0u || tag_len > AES_GCM_TAG_MAX_BYTES){
        return -1;
    }
    if (pt_len && (!pt || !ct)){
        return -1;
    }

    unsigned char j0[16];
    unsigned char ctr[16];
    unsigned char stream[16];
    unsigned char s[16];
    unsigned char e0[16];

    build_j0(key, iv, iv_len, j0);
    for (unsigned int i = 0; i < 16u; i++){
        ctr[i] = j0[i];
    }
    inc32(ctr);

    unsigned int off = 0;
    while (off < pt_len){
        aes_encrypt_block(key, ctr, stream);
        unsigned int rem = pt_len - off;
        unsigned int take = (rem < 16u) ? rem : 16u;
        for (unsigned int i = 0; i < take; i++){
            ct[off + i] = (unsigned char)(pt[off + i] ^ stream[i]);
        }
        off += take;
        inc32(ctr);
    }

    ghash_compute(s, key->h, aad, aad_len, ct, pt_len);
    aes_encrypt_block(key, j0, e0);
    xor_block(s, e0);
    for (unsigned int i = 0; i < tag_len; i++){
        tag[i] = s[i];
    }

    crypto_memzero(j0, sizeof(j0));
    crypto_memzero(ctr, sizeof(ctr));
    crypto_memzero(stream, sizeof(stream));
    crypto_memzero(s, sizeof(s));
    crypto_memzero(e0, sizeof(e0));
    return 0;
}

int aes_gcm_decrypt(const aes_gcm_key_t* key,
                    const unsigned char* iv, unsigned int iv_len,
                    const unsigned char* aad, unsigned int aad_len,
                    const unsigned char* ct, unsigned int ct_len,
                    unsigned char* pt,
                    const unsigned char* tag, unsigned int tag_len){
    if (!key || !iv || !tag){
        return -1;
    }
    if (tag_len == 0u || tag_len > AES_GCM_TAG_MAX_BYTES){
        return -1;
    }
    if (ct_len && (!ct || !pt)){
        return -1;
    }

    unsigned char j0[16];
    unsigned char ctr[16];
    unsigned char stream[16];
    unsigned char s[16];
    unsigned char e0[16];
    unsigned char expect[16];

    ghash_compute(s, key->h, aad, aad_len, ct, ct_len);
    build_j0(key, iv, iv_len, j0);
    aes_encrypt_block(key, j0, e0);
    xor_block(s, e0);
    for (unsigned int i = 0; i < tag_len; i++){
        expect[i] = s[i];
    }

    if (!crypto_consttime_equal(expect, tag, tag_len)){
        crypto_memzero(j0, sizeof(j0));
        crypto_memzero(ctr, sizeof(ctr));
        crypto_memzero(stream, sizeof(stream));
        crypto_memzero(s, sizeof(s));
        crypto_memzero(e0, sizeof(e0));
        crypto_memzero(expect, sizeof(expect));
        return -1;
    }

    for (unsigned int i = 0; i < 16u; i++){
        ctr[i] = j0[i];
    }
    inc32(ctr);

    unsigned int off = 0;
    while (off < ct_len){
        aes_encrypt_block(key, ctr, stream);
        unsigned int rem = ct_len - off;
        unsigned int take = (rem < 16u) ? rem : 16u;
        for (unsigned int i = 0; i < take; i++){
            pt[off + i] = (unsigned char)(ct[off + i] ^ stream[i]);
        }
        off += take;
        inc32(ctr);
    }

    crypto_memzero(j0, sizeof(j0));
    crypto_memzero(ctr, sizeof(ctr));
    crypto_memzero(stream, sizeof(stream));
    crypto_memzero(s, sizeof(s));
    crypto_memzero(e0, sizeof(e0));
    crypto_memzero(expect, sizeof(expect));
    return 0;
}

int aes_gcm_self_test(void){
    static const unsigned char k[16] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
    };
    static const unsigned char iv[12] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00
    };
    static const unsigned char pt[16] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
    };
    static const unsigned char exp_ct[16] = {
        0x03,0x88,0xDA,0xCE,0x60,0xB6,0xA3,0x92,
        0xF3,0x28,0xC2,0xB9,0x71,0xB2,0xFE,0x78
    };
    static const unsigned char exp_tag[16] = {
        0xAB,0x6E,0x47,0xD4,0x2C,0xEC,0x13,0xBD,
        0xF5,0x3A,0x67,0xB2,0x12,0x57,0xBD,0xDF
    };

    aes_gcm_key_t ks;
    unsigned char ct[16];
    unsigned char tag[16];
    unsigned char dec[16];

    if (aes_gcm_key_init(&ks, k, sizeof(k)) != 0){
        return -1;
    }
    if (aes_gcm_encrypt(&ks, iv, sizeof(iv), 0, 0, pt, sizeof(pt), ct, tag, sizeof(tag)) != 0){
        return -1;
    }
    if (!crypto_consttime_equal(ct, exp_ct, sizeof(ct))){
        return -1;
    }
    if (!crypto_consttime_equal(tag, exp_tag, sizeof(tag))){
        return -1;
    }
    if (aes_gcm_decrypt(&ks, iv, sizeof(iv), 0, 0, ct, sizeof(ct), dec, tag, sizeof(tag)) != 0){
        return -1;
    }
    if (!crypto_consttime_equal(dec, pt, sizeof(dec))){
        return -1;
    }
    return 0;
}
