#define ECC_CURVE secp256r1
#define EccPoint eccp256_point_impl
#define ecc_make_key eccp256_make_key
#define ecc_valid_public_key eccp256_valid_public_key
#define ecdh_shared_secret eccp256_shared_secret
#define ecdsa_sign eccp256_ecdsa_sign
#define ecdsa_verify eccp256_ecdsa_verify
#define ecc_bytes2native eccp256_bytes2native
#define ecc_native2bytes eccp256_native2bytes
#define ecc_point_compress eccp256_point_compress
#define ecc_point_decompress eccp256_point_decompress

#include "../third_party/micro_ecc/ecc.c"
