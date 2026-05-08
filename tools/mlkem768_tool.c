#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(QOS_HAVE_PQCLEAN_MLKEM768)
#include "../third_party/pqclean/crypto_kem/ml-kem-768/clean/api.h"
#define QOS_PQ_KEM_PUBLICKEYBYTES PQCLEAN_MLKEM768_CLEAN_CRYPTO_PUBLICKEYBYTES
#define QOS_PQ_KEM_SECRETKEYBYTES PQCLEAN_MLKEM768_CLEAN_CRYPTO_SECRETKEYBYTES
#define QOS_PQ_KEM_CIPHERTEXTBYTES PQCLEAN_MLKEM768_CLEAN_CRYPTO_CIPHERTEXTBYTES
#define QOS_PQ_KEM_BYTES PQCLEAN_MLKEM768_CLEAN_CRYPTO_BYTES
#define qos_pq_kem_keypair PQCLEAN_MLKEM768_CLEAN_crypto_kem_keypair
#define qos_pq_kem_encaps PQCLEAN_MLKEM768_CLEAN_crypto_kem_enc
#define qos_pq_kem_decaps PQCLEAN_MLKEM768_CLEAN_crypto_kem_dec
#elif defined(QOS_HAVE_PQCLEAN_KYBER768)
#include "../third_party/pqclean/crypto_kem/kyber768/clean/api.h"
#define QOS_PQ_KEM_PUBLICKEYBYTES PQCLEAN_KYBER768_CLEAN_CRYPTO_PUBLICKEYBYTES
#define QOS_PQ_KEM_SECRETKEYBYTES PQCLEAN_KYBER768_CLEAN_CRYPTO_SECRETKEYBYTES
#define QOS_PQ_KEM_CIPHERTEXTBYTES PQCLEAN_KYBER768_CLEAN_CRYPTO_CIPHERTEXTBYTES
#define QOS_PQ_KEM_BYTES PQCLEAN_KYBER768_CLEAN_CRYPTO_BYTES
#define qos_pq_kem_keypair PQCLEAN_KYBER768_CLEAN_crypto_kem_keypair
#define qos_pq_kem_encaps PQCLEAN_KYBER768_CLEAN_crypto_kem_enc
#define qos_pq_kem_decaps PQCLEAN_KYBER768_CLEAN_crypto_kem_dec
#else
#error "No ML-KEM/Kyber backend selected for mlkem768_tool"
#endif

static int read_file(const char* path, uint8_t** out, size_t* out_len){
    FILE* f = fopen(path, "rb");
    long sz;
    uint8_t* buf;
    size_t nread;
    if (!f){
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0){
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < 0){
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0){
        fclose(f);
        return -1;
    }
    buf = (uint8_t*)malloc((size_t)sz);
    if (!buf){
        fclose(f);
        return -1;
    }
    nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nread != (size_t)sz){
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

static int write_file(const char* path, const uint8_t* buf, size_t len){
    FILE* f = fopen(path, "wb");
    if (!f){
        return -1;
    }
    if (fwrite(buf, 1, len, f) != len){
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0){
        return -1;
    }
    return 0;
}

static int cmd_keygen(const char* pub_path, const char* priv_path){
    uint8_t pk[QOS_PQ_KEM_PUBLICKEYBYTES];
    uint8_t sk[QOS_PQ_KEM_SECRETKEYBYTES];
    int rc = -1;
    if (qos_pq_kem_keypair(pk, sk) != 0){
        goto out;
    }
    if (write_file(pub_path, pk, sizeof(pk)) != 0){
        goto out;
    }
    if (write_file(priv_path, sk, sizeof(sk)) != 0){
        goto out;
    }
    rc = 0;
out:
    memset(pk, 0, sizeof(pk));
    memset(sk, 0, sizeof(sk));
    return rc;
}

static int cmd_encap(const char* pub_path, const char* ct_path, const char* ss_path){
    uint8_t* pk = 0;
    size_t pk_len = 0;
    uint8_t ct[QOS_PQ_KEM_CIPHERTEXTBYTES];
    uint8_t ss[QOS_PQ_KEM_BYTES];
    int rc = -1;

    if (read_file(pub_path, &pk, &pk_len) != 0){
        goto out;
    }
    if (pk_len != QOS_PQ_KEM_PUBLICKEYBYTES){
        goto out;
    }
    if (qos_pq_kem_encaps(ct, ss, pk) != 0){
        goto out;
    }
    if (write_file(ct_path, ct, sizeof(ct)) != 0){
        goto out;
    }
    if (write_file(ss_path, ss, sizeof(ss)) != 0){
        goto out;
    }
    rc = 0;
out:
    if (pk){
        free(pk);
    }
    memset(ct, 0, sizeof(ct));
    memset(ss, 0, sizeof(ss));
    return rc;
}

static int cmd_decap(const char* priv_path, const char* ct_path, const char* ss_path){
    uint8_t* sk = 0;
    uint8_t* ct = 0;
    size_t sk_len = 0;
    size_t ct_len = 0;
    uint8_t ss[QOS_PQ_KEM_BYTES];
    int rc = -1;

    if (read_file(priv_path, &sk, &sk_len) != 0){
        goto out;
    }
    if (sk_len != QOS_PQ_KEM_SECRETKEYBYTES){
        goto out;
    }
    if (read_file(ct_path, &ct, &ct_len) != 0){
        goto out;
    }
    if (ct_len != QOS_PQ_KEM_CIPHERTEXTBYTES){
        goto out;
    }
    if (qos_pq_kem_decaps(ss, ct, sk) != 0){
        goto out;
    }
    if (write_file(ss_path, ss, sizeof(ss)) != 0){
        goto out;
    }
    rc = 0;
out:
    if (sk){
        memset(sk, 0, sk_len);
        free(sk);
    }
    if (ct){
        free(ct);
    }
    memset(ss, 0, sizeof(ss));
    return rc;
}

static void usage(void){
    printf("Usage:\n");
    printf("  mlkem768_tool keygen <pub.bin> <priv.bin>\n");
    printf("  mlkem768_tool encap <pub.bin> <ct.bin> <ss.bin>\n");
    printf("  mlkem768_tool decap <priv.bin> <ct.bin> <ss.bin>\n");
}

int main(int argc, char** argv){
    if (argc < 2){
        usage();
        return 1;
    }
    if (strcmp(argv[1], "keygen") == 0){
        if (argc != 4){
            usage();
            return 1;
        }
        return cmd_keygen(argv[2], argv[3]) == 0 ? 0 : 2;
    }
    if (strcmp(argv[1], "encap") == 0){
        if (argc != 5){
            usage();
            return 1;
        }
        return cmd_encap(argv[2], argv[3], argv[4]) == 0 ? 0 : 3;
    }
    if (strcmp(argv[1], "decap") == 0){
        if (argc != 5){
            usage();
            return 1;
        }
        return cmd_decap(argv[2], argv[3], argv[4]) == 0 ? 0 : 4;
    }
    usage();
    return 1;
}
