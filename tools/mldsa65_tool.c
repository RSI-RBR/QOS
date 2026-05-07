#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../third_party/pqclean/crypto_sign/ml-dsa-65/clean/api.h"

static int read_file(const char *path, uint8_t **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *buf;
    size_t nread;
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return -1;
    }
    nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nread != (size_t)sz) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

static int write_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        return -1;
    }
    return 0;
}

static int cmd_keygen(const char *pub_path, const char *priv_path) {
    uint8_t pk[PQCLEAN_MLDSA65_CLEAN_CRYPTO_PUBLICKEYBYTES];
    uint8_t sk[PQCLEAN_MLDSA65_CLEAN_CRYPTO_SECRETKEYBYTES];
    if (PQCLEAN_MLDSA65_CLEAN_crypto_sign_keypair(pk, sk) != 0) {
        return -1;
    }
    if (write_file(pub_path, pk, sizeof(pk)) != 0) {
        return -1;
    }
    if (write_file(priv_path, sk, sizeof(sk)) != 0) {
        return -1;
    }
    return 0;
}

static int cmd_sign(const char *priv_path, const char *msg_path, const char *sig_path) {
    uint8_t *sk = 0;
    uint8_t *msg = 0;
    size_t sk_len = 0;
    size_t msg_len = 0;
    uint8_t sig[PQCLEAN_MLDSA65_CLEAN_CRYPTO_BYTES];
    size_t sig_len = 0;
    int rc = -1;

    if (read_file(priv_path, &sk, &sk_len) != 0) {
        goto done;
    }
    if (sk_len != PQCLEAN_MLDSA65_CLEAN_CRYPTO_SECRETKEYBYTES) {
        goto done;
    }
    if (read_file(msg_path, &msg, &msg_len) != 0) {
        goto done;
    }
    if (PQCLEAN_MLDSA65_CLEAN_crypto_sign_signature(sig, &sig_len, msg, msg_len, sk) != 0) {
        goto done;
    }
    if (sig_len != PQCLEAN_MLDSA65_CLEAN_CRYPTO_BYTES) {
        goto done;
    }
    if (write_file(sig_path, sig, sig_len) != 0) {
        goto done;
    }
    rc = 0;

done:
    if (sk) {
        memset(sk, 0, sk_len);
        free(sk);
    }
    if (msg) {
        free(msg);
    }
    memset(sig, 0, sizeof(sig));
    return rc;
}

static int cmd_verify(const char *pub_path, const char *msg_path, const char *sig_path) {
    uint8_t *pk = 0;
    uint8_t *msg = 0;
    uint8_t *sig = 0;
    size_t pk_len = 0, msg_len = 0, sig_len = 0;
    int rc = -1;

    if (read_file(pub_path, &pk, &pk_len) != 0) {
        goto done;
    }
    if (pk_len != PQCLEAN_MLDSA65_CLEAN_CRYPTO_PUBLICKEYBYTES) {
        goto done;
    }
    if (read_file(msg_path, &msg, &msg_len) != 0) {
        goto done;
    }
    if (read_file(sig_path, &sig, &sig_len) != 0) {
        goto done;
    }
    if (sig_len != PQCLEAN_MLDSA65_CLEAN_CRYPTO_BYTES) {
        goto done;
    }
    if (PQCLEAN_MLDSA65_CLEAN_crypto_sign_verify(sig, sig_len, msg, msg_len, pk) != 0) {
        goto done;
    }
    rc = 0;

done:
    if (pk) {
        free(pk);
    }
    if (msg) {
        free(msg);
    }
    if (sig) {
        free(sig);
    }
    return rc;
}

static void usage(void) {
    printf("Usage:\n");
    printf("  mldsa65_tool keygen <pub.bin> <priv.bin>\n");
    printf("  mldsa65_tool sign <priv.bin> <msg.bin> <sig.bin>\n");
    printf("  mldsa65_tool verify <pub.bin> <msg.bin> <sig.bin>\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "keygen") == 0) {
        if (argc != 4) {
            usage();
            return 1;
        }
        return cmd_keygen(argv[2], argv[3]) == 0 ? 0 : 2;
    }
    if (strcmp(argv[1], "sign") == 0) {
        if (argc != 5) {
            usage();
            return 1;
        }
        return cmd_sign(argv[2], argv[3], argv[4]) == 0 ? 0 : 3;
    }
    if (strcmp(argv[1], "verify") == 0) {
        if (argc != 5) {
            usage();
            return 1;
        }
        return cmd_verify(argv[2], argv[3], argv[4]) == 0 ? 0 : 4;
    }
    usage();
    return 1;
}
