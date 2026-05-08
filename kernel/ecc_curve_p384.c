#define ECC_CURVE secp384r1
#define EccPoint eccp384_point_impl
#define ecc_make_key eccp384_make_key
#define ecc_valid_public_key eccp384_valid_public_key
#define ecdh_shared_secret eccp384_shared_secret
#define ecdsa_sign eccp384_ecdsa_sign
#define ecdsa_verify eccp384_ecdsa_verify
#define ecc_bytes2native eccp384_bytes2native
#define ecc_native2bytes eccp384_native2bytes
#define ecc_point_compress eccp384_point_compress
#define ecc_point_decompress eccp384_point_decompress

#include "../third_party/micro_ecc/ecc.c"
