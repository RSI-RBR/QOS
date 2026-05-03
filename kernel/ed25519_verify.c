#include "ed25519_verify.h"

// Vendored portable Ed25519 implementation (zlib license):
// third_party/ed25519 by Orson Peters, based on SUPERCOP ref10.
#include "ed25519.h"

int qos_ed25519_verify(const unsigned char signature[64],
                       const unsigned char* message,
                       unsigned int message_len,
                       const unsigned char public_key[32]){
    return ed25519_verify(signature, message, (size_t)message_len, public_key);
}
