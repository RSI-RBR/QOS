#include "trust.h"
#include "sha256.h"
#include "uart.h"

#define TRUST_ART_USER_APP 0u
#define TRUST_ART_SHELL    1u
#define TRUST_ART_WEB      2u

#define TRUST_KEY_ADMIN_MAIN 0x00000001u
#define TRUST_KEY_DEV_MAIN   0x00010001u

static const trust_key_t g_keys[] = {
    {
        TRUST_KEY_ADMIN_MAIN,
        "admin-main",
        TRUST_ROLE_ADMIN,
        TRUST_SCOPE_KERNEL | TRUST_SCOPE_SHELL | TRUST_SCOPE_WEB | TRUST_SCOPE_USER_APP,
        (1u << QOS_SIG_ALG_DIGEST_ONLY),
        0
    },
    {
        TRUST_KEY_DEV_MAIN,
        "dev-main",
        TRUST_ROLE_DEVELOPER,
        TRUST_SCOPE_USER_APP,
        (1u << QOS_SIG_ALG_DIGEST_ONLY),
        0
    }
};
static int g_warned_digest_only = 0;

static int fat_name_eq(const char* a, const char* b){
    if (!a || !b){
        return 0;
    }
    for (int i = 0; i < 11; i++){
        if (a[i] != b[i]){
            return 0;
        }
    }
    return 1;
}

static unsigned int classify_artifact(const char* fat_name_83){
    if (fat_name_eq(fat_name_83, "SHELL   BIN")){
        return TRUST_ART_SHELL;
    }
    if (fat_name_eq(fat_name_83, "WEBBROWSBIN")){
        return TRUST_ART_WEB;
    }
    return TRUST_ART_USER_APP;
}

const trust_key_t* trust_find_key(unsigned int key_id){
    for (unsigned int i = 0; i < (unsigned int)(sizeof(g_keys) / sizeof(g_keys[0])); i++){
        if (g_keys[i].key_id == key_id){
            return &g_keys[i];
        }
    }
    return 0;
}

static int has_scope(unsigned int scope_mask, unsigned int scope){
    return (scope_mask & scope) == scope;
}

int trust_verify_program_image(const char* fat_name_83,
                               const program_sec_header_t* sec,
                               const unsigned char* code,
                               unsigned int code_size){
    if (!sec){
        uart_puts("Trust: missing security header.\n");
        return -1;
    }
    if (sec->magic != QOS_SEC_MAGIC){
        uart_puts("Trust: bad security header magic.\n");
        return -1;
    }
    if (sec->header_size < sizeof(program_sec_header_t)){
        uart_puts("Trust: bad security header size.\n");
        return -1;
    }
    if (sec->sig_alg != QOS_SIG_ALG_DIGEST_ONLY){
        uart_puts("Trust: unsupported signature algorithm.\n");
        return -1;
    }
    if (!g_warned_digest_only){
        uart_puts("Trust: digest-only signatures enabled (development mode).\n");
        g_warned_digest_only = 1;
    }
    if ((sec->flags & QOS_PROG_FLAG_SHA256) == 0u){
        uart_puts("Trust: SHA-256 flag required.\n");
        return -1;
    }
    if (sec->sig_len != 0u){
        uart_puts("Trust: digest-only signature must have sig_len=0.\n");
        return -1;
    }

    const trust_key_t* key = trust_find_key(sec->signer_key_id);
    if (!key){
        uart_puts("Trust: signer key not trusted.\n");
        return -1;
    }
    if (key->revoked){
        uart_puts("Trust: signer key revoked.\n");
        return -1;
    }
    if ((key->sig_alg_mask & (1u << sec->sig_alg)) == 0u){
        uart_puts("Trust: signer key does not allow this signature algorithm.\n");
        return -1;
    }

    unsigned int artifact = classify_artifact(fat_name_83);
    unsigned int req_scope = TRUST_SCOPE_USER_APP;
    unsigned int req_role = TRUST_ROLE_DEVELOPER | TRUST_ROLE_ADMIN;
    if (artifact == TRUST_ART_SHELL){
        req_scope = TRUST_SCOPE_SHELL;
        req_role = TRUST_ROLE_ADMIN;
    } else if (artifact == TRUST_ART_WEB){
        req_scope = TRUST_SCOPE_WEB;
        req_role = TRUST_ROLE_ADMIN;
    }

    if (!has_scope(key->scope_mask, req_scope)){
        uart_puts("Trust: signer key missing required scope.\n");
        return -1;
    }
    if ((key->role_mask & req_role) == 0u){
        uart_puts("Trust: signer key missing required role.\n");
        return -1;
    }

    unsigned char digest[32];
    sha256_digest(code, code_size, digest);
    for (unsigned int i = 0; i < sizeof(digest); i++){
        if (digest[i] != sec->sha256[i]){
            uart_puts("Trust: program digest mismatch.\n");
            return -1;
        }
    }

    return 0;
}
