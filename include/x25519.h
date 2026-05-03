#ifndef X25519_H
#define X25519_H

int x25519_public_from_private(const unsigned char private_key[32],
                               unsigned char public_key_out[32]);

int x25519_shared_secret(const unsigned char private_key[32],
                         const unsigned char peer_public_key[32],
                         unsigned char shared_secret_out[32]);

int x25519_generate_private(unsigned char private_key_out[32]);
int x25519_generate_keypair(unsigned char private_key_out[32],
                            unsigned char public_key_out[32]);

int x25519_is_all_zero(const unsigned char k[32]);
int x25519_self_test(void);

#endif
