#ifndef TLS_SESSION_H
#define TLS_SESSION_H

#define QOS_TLS_ROLE_CLIENT 1
#define QOS_TLS_ROLE_SERVER 2

#define QOS_TLS_RECORD_INNER_HANDSHAKE 22u
#define QOS_TLS_RECORD_INNER_APPDATA 23u

typedef struct {
    unsigned char inner_type;
    const unsigned char* in;
    unsigned int in_len;
    unsigned char* out;
    unsigned int out_cap;
    unsigned int out_len;
} qos_tls_record_io_t;

void tls_session_layer_init(void);
void tls_session_close_all_for_pid(int pid);
int tls_session_self_test(void);

int ktls_open(int pid, int role);
int ktls_close(int pid, int tls_id);
int ktls_get_local_public(int pid, int tls_id, unsigned char out_public[32]);
int ktls_set_peer_public(int pid, int tls_id, const unsigned char peer_public[32]);

int ktls_build_client_hello(int pid, int tls_id,
                            unsigned char* out, unsigned int out_cap, unsigned int* out_len);
int ktls_process_server_hello(int pid, int tls_id,
                              const unsigned char* server_hello, unsigned int server_hello_len);
int ktls_process_client_hello_build_server_hello(int pid, int tls_id,
                                                 const unsigned char* client_hello, unsigned int client_hello_len,
                                                 unsigned char* out_server_hello, unsigned int out_cap, unsigned int* out_len);

int ktls_record_encrypt(int pid, int tls_id, qos_tls_record_io_t* io);
int ktls_record_decrypt(int pid, int tls_id, qos_tls_record_io_t* io);
int ktls_is_ready(int pid, int tls_id);

#endif
