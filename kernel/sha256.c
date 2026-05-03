#include "sha256.h"

static const unsigned int k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static unsigned int rotr(unsigned int x, unsigned int n){
    return (x >> n) | (x << (32u - n));
}

static unsigned int ch(unsigned int x, unsigned int y, unsigned int z){
    return (x & y) ^ (~x & z);
}

static unsigned int maj(unsigned int x, unsigned int y, unsigned int z){
    return (x & y) ^ (x & z) ^ (y & z);
}

static unsigned int bsig0(unsigned int x){
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

static unsigned int bsig1(unsigned int x){
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

static unsigned int ssig0(unsigned int x){
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

static unsigned int ssig1(unsigned int x){
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

static void sha256_transform(sha256_ctx_t* ctx, const unsigned char data[64]){
    unsigned int m[64];
    for (unsigned int i = 0; i < 16; i++){
        unsigned int j = i * 4u;
        m[i] = ((unsigned int)data[j] << 24) |
               ((unsigned int)data[j + 1u] << 16) |
               ((unsigned int)data[j + 2u] << 8) |
               ((unsigned int)data[j + 3u]);
    }
    for (unsigned int i = 16; i < 64; i++){
        m[i] = ssig1(m[i - 2u]) + m[i - 7u] + ssig0(m[i - 15u]) + m[i - 16u];
    }

    unsigned int a = ctx->h[0];
    unsigned int b = ctx->h[1];
    unsigned int c = ctx->h[2];
    unsigned int d = ctx->h[3];
    unsigned int e = ctx->h[4];
    unsigned int f = ctx->h[5];
    unsigned int g = ctx->h[6];
    unsigned int h = ctx->h[7];

    for (unsigned int i = 0; i < 64; i++){
        unsigned int t1 = h + bsig1(e) + ch(e, f, g) + k[i] + m[i];
        unsigned int t2 = bsig0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

void sha256_init(sha256_ctx_t* ctx){
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
}

void sha256_update(sha256_ctx_t* ctx, const unsigned char* data, unsigned int len){
    for (unsigned int i = 0; i < len; i++){
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64u){
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512u;
            ctx->datalen = 0;
        }
    }
}

void sha256_final(sha256_ctx_t* ctx, unsigned char out[32]){
    unsigned int i = ctx->datalen;

    if (ctx->datalen < 56u){
        ctx->data[i++] = 0x80u;
        while (i < 56u){
            ctx->data[i++] = 0;
        }
    } else{
        ctx->data[i++] = 0x80u;
        while (i < 64u){
            ctx->data[i++] = 0;
        }
        sha256_transform(ctx, ctx->data);
        for (i = 0; i < 56u; i++){
            ctx->data[i] = 0;
        }
    }

    ctx->bitlen += (unsigned long long)ctx->datalen * 8ull;
    ctx->data[63] = (unsigned char)(ctx->bitlen);
    ctx->data[62] = (unsigned char)(ctx->bitlen >> 8);
    ctx->data[61] = (unsigned char)(ctx->bitlen >> 16);
    ctx->data[60] = (unsigned char)(ctx->bitlen >> 24);
    ctx->data[59] = (unsigned char)(ctx->bitlen >> 32);
    ctx->data[58] = (unsigned char)(ctx->bitlen >> 40);
    ctx->data[57] = (unsigned char)(ctx->bitlen >> 48);
    ctx->data[56] = (unsigned char)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4u; i++){
        out[i]      = (unsigned char)((ctx->h[0] >> (24u - i * 8u)) & 0xFFu);
        out[i + 4u] = (unsigned char)((ctx->h[1] >> (24u - i * 8u)) & 0xFFu);
        out[i + 8u] = (unsigned char)((ctx->h[2] >> (24u - i * 8u)) & 0xFFu);
        out[i + 12u]= (unsigned char)((ctx->h[3] >> (24u - i * 8u)) & 0xFFu);
        out[i + 16u]= (unsigned char)((ctx->h[4] >> (24u - i * 8u)) & 0xFFu);
        out[i + 20u]= (unsigned char)((ctx->h[5] >> (24u - i * 8u)) & 0xFFu);
        out[i + 24u]= (unsigned char)((ctx->h[6] >> (24u - i * 8u)) & 0xFFu);
        out[i + 28u]= (unsigned char)((ctx->h[7] >> (24u - i * 8u)) & 0xFFu);
    }
}

void sha256_digest(const unsigned char* data, unsigned int len, unsigned char out[32]){
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void sha256_digest_concat2(const unsigned char* a, unsigned int a_len,
                           const unsigned char* b, unsigned int b_len,
                           unsigned char out[32]){
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    if (a && a_len){
        sha256_update(&ctx, a, a_len);
    }
    if (b && b_len){
        sha256_update(&ctx, b, b_len);
    }
    sha256_final(&ctx, out);
}
