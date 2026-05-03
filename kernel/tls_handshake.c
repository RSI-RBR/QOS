#include "tls_handshake.h"
#include "sha256.h"
#include "crypto.h"

static void be16_write(unsigned char* p, unsigned int v){
    p[0] = (unsigned char)((v >> 8) & 0xFFu);
    p[1] = (unsigned char)(v & 0xFFu);
}

static unsigned int be16_read(const unsigned char* p){
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static int write_extension_supported_versions_client(unsigned char* p, unsigned int cap, unsigned int* used){
    // extension_type(2) + extension_len(2) + versions_len(1) + version(2)
    if (!p || !used || cap < 7u){
        return -1;
    }
    be16_write(&p[0], 0x002Bu);
    be16_write(&p[2], 3u);
    p[4] = 2u;
    be16_write(&p[5], TLS13_VERSION_1_3);
    *used = 7u;
    return 0;
}

static int write_extension_key_share_client_x25519(const unsigned char client_pub[32],
                                                   unsigned char* p, unsigned int cap, unsigned int* used){
    // type(2) len(2) client_shares_len(2) group(2) kx_len(2) key(32)
    if (!client_pub || !p || !used || cap < 42u){
        return -1;
    }
    be16_write(&p[0], 0x0033u);
    be16_write(&p[2], 38u);
    be16_write(&p[4], 36u);
    be16_write(&p[6], TLS13_GROUP_X25519);
    be16_write(&p[8], 32u);
    for (unsigned int i = 0; i < 32u; i++){
        p[10u + i] = client_pub[i];
    }
    *used = 42u;
    return 0;
}

static int write_extension_supported_versions_server(unsigned char* p, unsigned int cap, unsigned int* used){
    // type(2) len(2) selected_version(2)
    if (!p || !used || cap < 6u){
        return -1;
    }
    be16_write(&p[0], 0x002Bu);
    be16_write(&p[2], 2u);
    be16_write(&p[4], TLS13_VERSION_1_3);
    *used = 6u;
    return 0;
}

static int write_extension_key_share_server_x25519(const unsigned char server_pub[32],
                                                   unsigned char* p, unsigned int cap, unsigned int* used){
    // type(2) len(2) group(2) kx_len(2) key(32)
    if (!server_pub || !p || !used || cap < 40u){
        return -1;
    }
    be16_write(&p[0], 0x0033u);
    be16_write(&p[2], 36u);
    be16_write(&p[4], TLS13_GROUP_X25519);
    be16_write(&p[6], 32u);
    for (unsigned int i = 0; i < 32u; i++){
        p[8u + i] = server_pub[i];
    }
    *used = 40u;
    return 0;
}

int tls13_build_client_hello_x25519(const unsigned char client_pub[32],
                                    unsigned char* out, unsigned int out_cap, unsigned int* out_len){
    if (!client_pub || !out || !out_len){
        return -1;
    }

    unsigned int min_body = 2u + 32u + 1u + 2u + 2u + 1u + 1u + 2u + 7u + 42u;
    unsigned int total = 4u + min_body;
    if (out_cap < total){
        return -1;
    }

    unsigned int i = 0;
    out[i++] = (unsigned char)TLS13_HS_TYPE_CLIENT_HELLO;
    out[i++] = 0; out[i++] = 0; out[i++] = 0; // body len later

    be16_write(&out[i], TLS13_VERSION_LEGACY); i += 2u;
    for (unsigned int r = 0; r < 32u; r++){
        out[i++] = (unsigned char)(0xA0u + r); // deterministic scaffold random
    }
    out[i++] = 0u; // legacy_session_id length

    be16_write(&out[i], 2u); i += 2u;
    be16_write(&out[i], TLS13_CIPHER_AES_128_GCM_SHA256); i += 2u;

    out[i++] = 1u; // compression methods len
    out[i++] = 0u; // null compression

    unsigned int ext_len_pos = i;
    i += 2u;
    unsigned int ext_used = 0;

    unsigned int n = 0;
    if (write_extension_supported_versions_client(&out[i], out_cap - i, &n) != 0){
        return -1;
    }
    i += n; ext_used += n;

    if (write_extension_key_share_client_x25519(client_pub, &out[i], out_cap - i, &n) != 0){
        return -1;
    }
    i += n; ext_used += n;

    be16_write(&out[ext_len_pos], ext_used);

    unsigned int body_len = i - 4u;
    out[1] = (unsigned char)((body_len >> 16) & 0xFFu);
    out[2] = (unsigned char)((body_len >> 8) & 0xFFu);
    out[3] = (unsigned char)(body_len & 0xFFu);
    *out_len = i;
    return 0;
}

static int parse_key_share_from_hello(const unsigned char* msg, unsigned int msg_len,
                                      unsigned int expected_hs_type,
                                      unsigned int key_share_ext_kind_client,
                                      unsigned char pub_out[32]){
    if (!msg || !pub_out || msg_len < 4u){
        return -1;
    }
    if ((unsigned int)msg[0] != expected_hs_type){
        return -1;
    }
    unsigned int body_len = ((unsigned int)msg[1] << 16) | ((unsigned int)msg[2] << 8) | (unsigned int)msg[3];
    if (body_len + 4u != msg_len){
        return -1;
    }
    unsigned int p = 4u;
    p += 2u;      // legacy_version
    p += 32u;     // random
    if (p + 1u > msg_len){
        return -1;
    }
    unsigned int sid_len = msg[p++];
    if (p + sid_len > msg_len){
        return -1;
    }
    p += sid_len;

    if (expected_hs_type == TLS13_HS_TYPE_CLIENT_HELLO){
        if (p + 2u > msg_len){
            return -1;
        }
        unsigned int cs_len = be16_read(&msg[p]); p += 2u;
        if (cs_len < 2u || (cs_len & 1u) != 0u || p + cs_len > msg_len){
            return -1;
        }
        p += cs_len;

        if (p + 1u > msg_len){
            return -1;
        }
        unsigned int comp_len = msg[p++];
        if (p + comp_len > msg_len){
            return -1;
        }
        p += comp_len;
    } else{
        // ServerHello: selected cipher suite (2), compression method (1)
        if (p + 3u > msg_len){
            return -1;
        }
        unsigned int cs = be16_read(&msg[p]); p += 2u;
        if (cs != TLS13_CIPHER_AES_128_GCM_SHA256){
            return -1;
        }
        p += 1u; // compression
    }

    if (p + 2u > msg_len){
        return -1;
    }
    unsigned int ext_total = be16_read(&msg[p]); p += 2u;
    if (p + ext_total > msg_len){
        return -1;
    }
    unsigned int ext_end = p + ext_total;

    while (p + 4u <= ext_end){
        unsigned int ext_type = be16_read(&msg[p]); p += 2u;
        unsigned int ext_len = be16_read(&msg[p]); p += 2u;
        if (p + ext_len > ext_end){
            return -1;
        }

        if (ext_type == 0x0033u){
            if (key_share_ext_kind_client){
                // ClientHello key_share: vector length + first KeyShareEntry.
                if (ext_len < 2u + 2u + 2u + 32u){
                    return -1;
                }
                unsigned int q = p;
                unsigned int shares_len = be16_read(&msg[q]); q += 2u;
                if (shares_len + 2u > ext_len){
                    return -1;
                }
                unsigned int group = be16_read(&msg[q]); q += 2u;
                unsigned int kx_len = be16_read(&msg[q]); q += 2u;
                if (group != TLS13_GROUP_X25519 || kx_len != 32u || q + 32u > p + ext_len){
                    return -1;
                }
                for (unsigned int i = 0; i < 32u; i++){
                    pub_out[i] = msg[q + i];
                }
                return 0;
            } else{
                // ServerHello key_share: single KeyShareEntry.
                if (ext_len < 2u + 2u + 32u){
                    return -1;
                }
                unsigned int q = p;
                unsigned int group = be16_read(&msg[q]); q += 2u;
                unsigned int kx_len = be16_read(&msg[q]); q += 2u;
                if (group != TLS13_GROUP_X25519 || kx_len != 32u || q + 32u > p + ext_len){
                    return -1;
                }
                for (unsigned int i = 0; i < 32u; i++){
                    pub_out[i] = msg[q + i];
                }
                return 0;
            }
        }

        p += ext_len;
    }

    return -1;
}

static int build_server_hello_x25519(const unsigned char server_pub[32],
                                     unsigned char* out, unsigned int out_cap, unsigned int* out_len){
    if (!server_pub || !out || !out_len){
        return -1;
    }
    unsigned int min_body = 2u + 32u + 1u + 2u + 1u + 2u + 6u + 40u;
    unsigned int total = 4u + min_body;
    if (out_cap < total){
        return -1;
    }

    unsigned int i = 0;
    out[i++] = (unsigned char)TLS13_HS_TYPE_SERVER_HELLO;
    out[i++] = 0; out[i++] = 0; out[i++] = 0;

    be16_write(&out[i], TLS13_VERSION_LEGACY); i += 2u;
    for (unsigned int r = 0; r < 32u; r++){
        out[i++] = (unsigned char)(0xC0u + r); // deterministic scaffold random
    }
    out[i++] = 0u; // legacy_session_id_echo len
    be16_write(&out[i], TLS13_CIPHER_AES_128_GCM_SHA256); i += 2u;
    out[i++] = 0u; // compression method

    unsigned int ext_len_pos = i;
    i += 2u;
    unsigned int ext_used = 0;
    unsigned int n = 0;

    if (write_extension_supported_versions_server(&out[i], out_cap - i, &n) != 0){
        return -1;
    }
    i += n; ext_used += n;

    if (write_extension_key_share_server_x25519(server_pub, &out[i], out_cap - i, &n) != 0){
        return -1;
    }
    i += n; ext_used += n;

    be16_write(&out[ext_len_pos], ext_used);
    unsigned int body_len = i - 4u;
    out[1] = (unsigned char)((body_len >> 16) & 0xFFu);
    out[2] = (unsigned char)((body_len >> 8) & 0xFFu);
    out[3] = (unsigned char)(body_len & 0xFFu);
    *out_len = i;
    return 0;
}

int tls13_process_client_hello_and_build_server_hello_x25519(
    const unsigned char* client_hello, unsigned int client_hello_len,
    unsigned char client_pub_out[32],
    const unsigned char server_pub[32],
    unsigned char* out_server_hello, unsigned int out_cap, unsigned int* out_len){
    if (!client_hello || !client_pub_out || !server_pub || !out_server_hello || !out_len){
        return -1;
    }
    if (parse_key_share_from_hello(client_hello, client_hello_len,
                                   TLS13_HS_TYPE_CLIENT_HELLO, 1u, client_pub_out) != 0){
        return -1;
    }
    return build_server_hello_x25519(server_pub, out_server_hello, out_cap, out_len);
}

int tls13_process_server_hello_x25519(const unsigned char* server_hello, unsigned int server_hello_len,
                                      unsigned char server_pub_out[32]){
    return parse_key_share_from_hello(server_hello, server_hello_len,
                                      TLS13_HS_TYPE_SERVER_HELLO, 0u, server_pub_out);
}

int tls13_transcript_hash2(const unsigned char* m1, unsigned int m1_len,
                           const unsigned char* m2, unsigned int m2_len,
                           unsigned char out_hash[32]){
    if (!m1 || !m2 || !out_hash){
        return -1;
    }
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, m1, m1_len);
    sha256_update(&ctx, m2, m2_len);
    sha256_final(&ctx, out_hash);
    return 0;
}

int tls13_handshake_self_test(void){
    unsigned char cpub[32];
    unsigned char spub[32];
    unsigned char ch[256];
    unsigned char sh[256];
    unsigned char cpub2[32];
    unsigned char spub2[32];
    unsigned int ch_len = 0;
    unsigned int sh_len = 0;
    unsigned char th[32];

    for (unsigned int i = 0; i < 32u; i++){
        cpub[i] = (unsigned char)(0x11u + i);
        spub[i] = (unsigned char)(0x55u + i);
    }

    if (tls13_build_client_hello_x25519(cpub, ch, sizeof(ch), &ch_len) != 0){
        return -1;
    }
    if (tls13_process_client_hello_and_build_server_hello_x25519(ch, ch_len, cpub2,
                                                                  spub, sh, sizeof(sh), &sh_len) != 0){
        return -2;
    }
    if (!crypto_consttime_equal(cpub, cpub2, 32u)){
        return -3;
    }
    if (tls13_process_server_hello_x25519(sh, sh_len, spub2) != 0){
        return -4;
    }
    if (!crypto_consttime_equal(spub, spub2, 32u)){
        return -5;
    }
    if (tls13_transcript_hash2(ch, ch_len, sh, sh_len, th) != 0){
        return -6;
    }
    unsigned char nz = 0;
    for (unsigned int i = 0; i < sizeof(th); i++){
        nz |= th[i];
    }
    if (nz == 0u){ // Should never happen in this deterministic test.
        return -7;
    }
    return 0;
}
