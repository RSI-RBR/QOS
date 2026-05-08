#ifndef ARGON2_KDF_H
#define ARGON2_KDF_H

int argon2id_hash_raw_qos(unsigned int t_cost,
                          unsigned int m_cost_kib,
                          unsigned int parallelism,
                          const void* password,
                          unsigned int password_len,
                          const void* salt,
                          unsigned int salt_len,
                          void* out_hash,
                          unsigned int out_hash_len,
                          unsigned int version);

#endif
