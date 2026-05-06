#ifndef AUTH_H
#define AUTH_H

#define AUTH_SALT_BYTES 16u
#define AUTH_HASH_BYTES 32u
#define AUTH_NONCE_BYTES 32u
#define AUTH_USERNAME_MAX 31u

int auth_init(void);
int auth_is_ready(void);
const char* auth_username(void);
int auth_get_salt(unsigned char out_salt[AUTH_SALT_BYTES]);
int auth_issue_nonce(unsigned char out_nonce[AUTH_NONCE_BYTES]);

void create_password_hash(const char* password,
                          const unsigned char salt[AUTH_SALT_BYTES],
                          unsigned char out_hash[AUTH_HASH_BYTES]);

int auth_verify_response(const char* username,
                         const unsigned char client_nonce[AUTH_NONCE_BYTES],
                         const unsigned char server_nonce[AUTH_NONCE_BYTES],
                         const unsigned char response[AUTH_HASH_BYTES]);

#endif
