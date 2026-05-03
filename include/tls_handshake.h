#ifndef TLS_HANDSHAKE_H
#define TLS_HANDSHAKE_H

#define TLS13_HS_TYPE_CLIENT_HELLO 1u
#define TLS13_HS_TYPE_SERVER_HELLO 2u

#define TLS13_GROUP_X25519 0x001Du
#define TLS13_VERSION_1_3 0x0304u
#define TLS13_VERSION_LEGACY 0x0303u
#define TLS13_CIPHER_AES_128_GCM_SHA256 0x1301u

int tls13_build_client_hello_x25519(const unsigned char client_pub[32],
                                    unsigned char* out, unsigned int out_cap, unsigned int* out_len);

int tls13_process_client_hello_and_build_server_hello_x25519(
    const unsigned char* client_hello, unsigned int client_hello_len,
    unsigned char client_pub_out[32],
    const unsigned char server_pub[32],
    unsigned char* out_server_hello, unsigned int out_cap, unsigned int* out_len);

int tls13_process_server_hello_x25519(const unsigned char* server_hello, unsigned int server_hello_len,
                                      unsigned char server_pub_out[32]);

int tls13_transcript_hash2(const unsigned char* m1, unsigned int m1_len,
                           const unsigned char* m2, unsigned int m2_len,
                           unsigned char out_hash[32]);

int tls13_handshake_self_test(void);

#endif
