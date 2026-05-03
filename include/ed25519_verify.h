#ifndef ED25519_VERIFY_H
#define ED25519_VERIFY_H

// Returns 1 on valid signature, 0 on invalid.
int qos_ed25519_verify(const unsigned char signature[64],
                       const unsigned char* message,
                       unsigned int message_len,
                       const unsigned char public_key[32]);

#endif
