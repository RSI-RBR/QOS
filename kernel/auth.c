#include "auth.h"
#include "fat32.h"
#include "sha256.h"
#include "crypto.h"
#include "uart.h"

#define AUTH_FILE_83 "AUTH    BIN"

#define AUTH_MAGIC_BYTES 8u
#define AUTH_VERSION 1u
#define AUTH_FILE_MIN_BYTES (AUTH_MAGIC_BYTES + 1u + 1u + (AUTH_USERNAME_MAX + 1u) + AUTH_SALT_BYTES + AUTH_HASH_BYTES)

typedef struct {
    int ready;
    unsigned int username_len;
    char username[AUTH_USERNAME_MAX + 1u];
    unsigned char salt[AUTH_SALT_BYTES];
    unsigned char stored_hash[AUTH_HASH_BYTES];
} auth_state_t;

static auth_state_t g_auth;

static const unsigned char k_auth_magic[AUTH_MAGIC_BYTES] = {
    'Q','A','U','T','H','V','1','\0'
};

static void auth_zero_state(void){
    g_auth.ready = 0;
    g_auth.username_len = 0;
    for (unsigned int i = 0; i < sizeof(g_auth.username); i++){
        g_auth.username[i] = 0;
    }
    crypto_memzero(g_auth.salt, sizeof(g_auth.salt));
    crypto_memzero(g_auth.stored_hash, sizeof(g_auth.stored_hash));
}

static unsigned int cstr_len_bounded(const char* s, unsigned int cap){
    unsigned int n = 0;
    if (!s){
        return 0;
    }
    while (n < cap && s[n]){
        n++;
    }
    return n;
}

static int bytes_eq(const unsigned char* a, const unsigned char* b, unsigned int n){
    unsigned char d = 0;
    for (unsigned int i = 0; i < n; i++){
        d |= (unsigned char)(a[i] ^ b[i]);
    }
    return d == 0;
}

void create_password_hash(const char* password,
                          const unsigned char salt[AUTH_SALT_BYTES],
                          unsigned char out_hash[AUTH_HASH_BYTES]){
    sha256_ctx_t ctx;
    unsigned int pw_len = cstr_len_bounded(password, 1024u);
    sha256_init(&ctx);
    if (password && pw_len){
        sha256_update(&ctx, (const unsigned char*)password, pw_len);
    }
    if (salt){
        sha256_update(&ctx, salt, AUTH_SALT_BYTES);
    }
    sha256_final(&ctx, out_hash);
}

static void compute_expected_response(const unsigned char client_nonce[AUTH_NONCE_BYTES],
                                      const unsigned char server_nonce[AUTH_NONCE_BYTES],
                                      unsigned char out_hash[AUTH_HASH_BYTES]){
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, g_auth.stored_hash, AUTH_HASH_BYTES);
    sha256_update(&ctx, client_nonce, AUTH_NONCE_BYTES);
    sha256_update(&ctx, server_nonce, AUTH_NONCE_BYTES);
    sha256_final(&ctx, out_hash);
}

int auth_init(void){
    static unsigned char buf[256];
    int n;
    unsigned int off;

    auth_zero_state();

    if (fat32_init() != 0){
        uart_puts("AUTH: FAT init failed\n");
        return -1;
    }

    n = fat32_read_file(AUTH_FILE_83, buf, sizeof(buf));
    if (n < (int)AUTH_FILE_MIN_BYTES){
        uart_puts("AUTH: file missing or too small\n");
        return -1;
    }

    off = 0u;
    if (!bytes_eq(&buf[off], k_auth_magic, AUTH_MAGIC_BYTES)){
        uart_puts("AUTH: bad magic\n");
        return -1;
    }
    off += AUTH_MAGIC_BYTES;

    if (buf[off++] != AUTH_VERSION){
        uart_puts("AUTH: bad version\n");
        return -1;
    }

    g_auth.username_len = (unsigned int)buf[off++];
    if (g_auth.username_len == 0u || g_auth.username_len > AUTH_USERNAME_MAX){
        uart_puts("AUTH: bad username length\n");
        return -1;
    }

    for (unsigned int i = 0; i < AUTH_USERNAME_MAX + 1u; i++){
        unsigned char ch = buf[off + i];
        g_auth.username[i] = (i < g_auth.username_len) ? (char)ch : 0;
    }
    g_auth.username[g_auth.username_len] = 0;
    off += (AUTH_USERNAME_MAX + 1u);

    for (unsigned int i = 0; i < AUTH_SALT_BYTES; i++){
        g_auth.salt[i] = buf[off + i];
    }
    off += AUTH_SALT_BYTES;

    for (unsigned int i = 0; i < AUTH_HASH_BYTES; i++){
        g_auth.stored_hash[i] = buf[off + i];
    }

    g_auth.ready = 1;
    uart_puts("AUTH: loaded user ");
    uart_puts(g_auth.username);
    uart_puts("\n");
    return 0;
}

int auth_is_ready(void){
    return g_auth.ready;
}

const char* auth_username(void){
    if (!g_auth.ready){
        return "";
    }
    return g_auth.username;
}

int auth_get_salt(unsigned char out_salt[AUTH_SALT_BYTES]){
    if (!out_salt || !g_auth.ready){
        return -1;
    }
    for (unsigned int i = 0; i < AUTH_SALT_BYTES; i++){
        out_salt[i] = g_auth.salt[i];
    }
    return 0;
}

int auth_issue_nonce(unsigned char out_nonce[AUTH_NONCE_BYTES]){
    if (!out_nonce){
        return -1;
    }
    if (crypto_random_bytes(out_nonce, AUTH_NONCE_BYTES) != 0){
        return -1;
    }
    return 0;
}

int auth_verify_response(const char* username,
                         const unsigned char client_nonce[AUTH_NONCE_BYTES],
                         const unsigned char server_nonce[AUTH_NONCE_BYTES],
                         const unsigned char response[AUTH_HASH_BYTES]){
    unsigned char expected[AUTH_HASH_BYTES];
    unsigned int name_len;

    if (!g_auth.ready || !username || !client_nonce || !server_nonce || !response){
        return -1;
    }

    name_len = cstr_len_bounded(username, AUTH_USERNAME_MAX + 1u);
    if (name_len != g_auth.username_len){
        return -1;
    }
    for (unsigned int i = 0; i < name_len; i++){
        if (username[i] != g_auth.username[i]){
            return -1;
        }
    }

    compute_expected_response(client_nonce, server_nonce, expected);
    int ok = crypto_consttime_equal(expected, response, AUTH_HASH_BYTES);
    crypto_memzero(expected, sizeof(expected));
    return ok ? 0 : -1;
}
