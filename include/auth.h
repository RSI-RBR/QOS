#ifndef AUTH_H
#define AUTH_H

#define AUTH_SALT_BYTES 16u
#define AUTH_HASH_BYTES 32u
#define AUTH_NONCE_BYTES 32u
#define AUTH_USERNAME_MAX 31u

#define AUTH_KDF_SHA256 1u
#define AUTH_KDF_ARGON2ID 2u

#define AUTH_ARGON2_DEFAULT_T_COST 3u
#define AUTH_ARGON2_DEFAULT_M_COST_KIB 4096u
#define AUTH_ARGON2_DEFAULT_PARALLELISM 1u
#define AUTH_ARGON2_DEFAULT_VERSION 0x13u

typedef struct {
    unsigned char kdf_id;
    unsigned int argon2_t_cost;
    unsigned int argon2_m_cost_kib;
    unsigned int argon2_parallelism;
    unsigned int argon2_version;
} auth_kdf_info_t;

int auth_init(void);
int auth_is_ready(void);
const char* auth_username(void);
int auth_get_salt(unsigned char out_salt[AUTH_SALT_BYTES]);
int auth_issue_nonce(unsigned char out_nonce[AUTH_NONCE_BYTES]);
int auth_get_kdf_info(auth_kdf_info_t* out_info);

int auth_verify_password(const char* username, const char* password);

void create_password_hash(const char* password,
                          const unsigned char salt[AUTH_SALT_BYTES],
                          unsigned char out_hash[AUTH_HASH_BYTES]);

int auth_verify_response(const char* username,
                         const unsigned char client_nonce[AUTH_NONCE_BYTES],
                         const unsigned char server_nonce[AUTH_NONCE_BYTES],
                         const unsigned char response[AUTH_HASH_BYTES]);

#endif
