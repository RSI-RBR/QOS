#include "tls_session.h"
#include "process.h"
#include "spinlock.h"
#include "crypto.h"
#include "x25519.h"
#include "tls_handshake.h"
#include "tls_key_schedule.h"
#include "tls_record.h"

#define TLS_MAX_GLOBAL 16
#define TLS_MAX_PER_PROCESS 4
#define TLS_CLIENT_HELLO_MAX 256

typedef struct {
    int used;
    int owner_pid;
    int role;
    int ready;
    int has_peer;
    unsigned char local_private[32];
    unsigned char local_public[32];
    unsigned char peer_public[32];
    unsigned char shared_secret[32];
    unsigned char last_client_hello[TLS_CLIENT_HELLO_MAX];
    unsigned int last_client_hello_len;
    tls13_hs_secrets_t hs;
    tls13_record_ctx_t tx;
    tls13_record_ctx_t rx;
} tls_session_t;

static tls_session_t g_tls_sessions[TLS_MAX_GLOBAL];
static int g_tls_fd_map[MAX_PROCESSES][TLS_MAX_PER_PROCESS];
static spinlock_t g_tls_lock;

static int valid_pid(int pid){
    return pid >= 0 && pid < MAX_PROCESSES;
}

static int valid_tls_fd(int fd){
    return fd >= 0 && fd < TLS_MAX_PER_PROCESS;
}

static void clear_session(tls_session_t* s){
    if (!s){
        return;
    }
    s->used = 0;
    s->owner_pid = -1;
    s->role = 0;
    s->ready = 0;
    s->has_peer = 0;
    crypto_memzero(s->local_private, sizeof(s->local_private));
    crypto_memzero(s->local_public, sizeof(s->local_public));
    crypto_memzero(s->peer_public, sizeof(s->peer_public));
    crypto_memzero(s->shared_secret, sizeof(s->shared_secret));
    crypto_memzero(s->last_client_hello, sizeof(s->last_client_hello));
    s->last_client_hello_len = 0;
    crypto_memzero(&s->hs, sizeof(s->hs));
    crypto_memzero(&s->tx, sizeof(s->tx));
    crypto_memzero(&s->rx, sizeof(s->rx));
}

static int alloc_global_session_locked(void){
    for (int i = 0; i < TLS_MAX_GLOBAL; i++){
        if (!g_tls_sessions[i].used){
            g_tls_sessions[i].used = 1;
            return i;
        }
    }
    return -1;
}

static int alloc_proc_fd_locked(int pid){
    for (int i = 0; i < TLS_MAX_PER_PROCESS; i++){
        if (g_tls_fd_map[pid][i] < 0){
            return i;
        }
    }
    return -1;
}

static int lookup_locked(int pid, int tls_id){
    if (!valid_pid(pid) || !valid_tls_fd(tls_id)){
        return -1;
    }
    int si = g_tls_fd_map[pid][tls_id];
    if (si < 0 || si >= TLS_MAX_GLOBAL){
        return -1;
    }
    if (!g_tls_sessions[si].used || g_tls_sessions[si].owner_pid != pid){
        return -1;
    }
    return si;
}

static void sync_user_read(const void* p, unsigned int len){
    if (p && len){
        asm volatile("dmb ish" : : : "memory");
    }
}

static void sync_user_write(const void* p, unsigned int len){
    if (p && len){
        asm volatile("dmb ishst" : : : "memory");
    }
}

static int derive_locked(tls_session_t* s,
                         const unsigned char* transcript_hash_32){
    if (!s || !transcript_hash_32 || !s->has_peer){
        return -1;
    }
    if (x25519_shared_secret(s->local_private, s->peer_public, s->shared_secret) != 0){
        return -1;
    }
    if (tls13_derive_handshake_secrets_sha256(s->shared_secret, transcript_hash_32, &s->hs) != 0){
        return -1;
    }

    if (s->role == QOS_TLS_ROLE_CLIENT){
        if (tls13_record_init(&s->tx, s->hs.client_key, TLS13_KEY_BYTES, s->hs.client_iv) != 0){
            return -1;
        }
        if (tls13_record_init(&s->rx, s->hs.server_key, TLS13_KEY_BYTES, s->hs.server_iv) != 0){
            return -1;
        }
    } else{
        if (tls13_record_init(&s->tx, s->hs.server_key, TLS13_KEY_BYTES, s->hs.server_iv) != 0){
            return -1;
        }
        if (tls13_record_init(&s->rx, s->hs.client_key, TLS13_KEY_BYTES, s->hs.client_iv) != 0){
            return -1;
        }
    }
    s->ready = 1;
    return 0;
}

void tls_session_layer_init(void){
    spinlock_init(&g_tls_lock);
    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    for (int i = 0; i < TLS_MAX_GLOBAL; i++){
        clear_session(&g_tls_sessions[i]);
    }
    for (int p = 0; p < MAX_PROCESSES; p++){
        for (int i = 0; i < TLS_MAX_PER_PROCESS; i++){
            g_tls_fd_map[p][i] = -1;
        }
    }
    spin_unlock_irqrestore(&g_tls_lock, irq);
}

void tls_session_close_all_for_pid(int pid){
    if (!valid_pid(pid)){
        return;
    }
    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    for (int i = 0; i < TLS_MAX_PER_PROCESS; i++){
        int si = g_tls_fd_map[pid][i];
        if (si >= 0 && si < TLS_MAX_GLOBAL){
            clear_session(&g_tls_sessions[si]);
        }
        g_tls_fd_map[pid][i] = -1;
    }
    spin_unlock_irqrestore(&g_tls_lock, irq);
}

int ktls_open(int pid, int role){
    if (!valid_pid(pid)){
        return -1;
    }
    if (role != QOS_TLS_ROLE_CLIENT && role != QOS_TLS_ROLE_SERVER){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = alloc_global_session_locked();
    if (si < 0){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    int fd = alloc_proc_fd_locked(pid);
    if (fd < 0){
        clear_session(&g_tls_sessions[si]);
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }

    tls_session_t* s = &g_tls_sessions[si];
    s->owner_pid = pid;
    s->role = role;
    s->ready = 0;
    s->has_peer = 0;

    if (x25519_generate_keypair(s->local_private, s->local_public) != 0){
        clear_session(s);
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    g_tls_fd_map[pid][fd] = si;
    spin_unlock_irqrestore(&g_tls_lock, irq);
    return fd;
}

int ktls_close(int pid, int tls_id){
    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = lookup_locked(pid, tls_id);
    if (si < 0){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    clear_session(&g_tls_sessions[si]);
    g_tls_fd_map[pid][tls_id] = -1;
    spin_unlock_irqrestore(&g_tls_lock, irq);
    return 0;
}

int ktls_get_local_public(int pid, int tls_id, unsigned char out_public[32]){
    if (!out_public){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = lookup_locked(pid, tls_id);
    if (si < 0){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    for (unsigned int i = 0; i < 32u; i++){
        out_public[i] = g_tls_sessions[si].local_public[i];
    }
    spin_unlock_irqrestore(&g_tls_lock, irq);
    sync_user_write(out_public, 32u);
    return 0;
}

int ktls_set_peer_public(int pid, int tls_id, const unsigned char peer_public[32]){
    if (!peer_public){
        return -1;
    }
    sync_user_read(peer_public, 32u);
    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = lookup_locked(pid, tls_id);
    if (si < 0){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    for (unsigned int i = 0; i < 32u; i++){
        g_tls_sessions[si].peer_public[i] = peer_public[i];
    }
    g_tls_sessions[si].has_peer = 1;
    g_tls_sessions[si].ready = 0;
    spin_unlock_irqrestore(&g_tls_lock, irq);
    return 0;
}

int ktls_build_client_hello(int pid, int tls_id,
                            unsigned char* out, unsigned int out_cap, unsigned int* out_len){
    if (!out || !out_len){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = lookup_locked(pid, tls_id);
    if (si < 0 || g_tls_sessions[si].role != QOS_TLS_ROLE_CLIENT){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    unsigned char pub[32];
    for (unsigned int i = 0; i < 32u; i++){
        pub[i] = g_tls_sessions[si].local_public[i];
    }
    spin_unlock_irqrestore(&g_tls_lock, irq);

    int rc = tls13_build_client_hello_x25519(pub, out, out_cap, out_len);
    if (rc != 0){
        return -1;
    }
    if (*out_len > TLS_CLIENT_HELLO_MAX){
        return -1;
    }

    irq = spin_lock_irqsave(&g_tls_lock);
    si = lookup_locked(pid, tls_id);
    if (si < 0 || g_tls_sessions[si].role != QOS_TLS_ROLE_CLIENT){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    tls_session_t* s = &g_tls_sessions[si];
    for (unsigned int i = 0; i < *out_len; i++){
        s->last_client_hello[i] = out[i];
    }
    s->last_client_hello_len = *out_len;
    spin_unlock_irqrestore(&g_tls_lock, irq);
    sync_user_write(out, *out_len);
    return 0;
}

int ktls_process_server_hello(int pid, int tls_id,
                              const unsigned char* server_hello, unsigned int server_hello_len){
    if (!server_hello){
        return -1;
    }
    sync_user_read(server_hello, server_hello_len);
    unsigned char peer_pub[32];
    if (tls13_process_server_hello_x25519(server_hello, server_hello_len, peer_pub) != 0){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = lookup_locked(pid, tls_id);
    if (si < 0 || g_tls_sessions[si].role != QOS_TLS_ROLE_CLIENT){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    tls_session_t* s = &g_tls_sessions[si];
    for (unsigned int i = 0; i < 32u; i++){
        s->peer_public[i] = peer_pub[i];
    }
    s->has_peer = 1;

    if (s->last_client_hello_len == 0u || s->last_client_hello_len > TLS_CLIENT_HELLO_MAX){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }

    unsigned char th[32];
    if (tls13_transcript_hash2(s->last_client_hello, s->last_client_hello_len,
                               server_hello, server_hello_len, th) != 0){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    int rc = derive_locked(s, th);
    spin_unlock_irqrestore(&g_tls_lock, irq);
    return rc;
}

int ktls_process_client_hello_build_server_hello(int pid, int tls_id,
                                                 const unsigned char* client_hello, unsigned int client_hello_len,
                                                 unsigned char* out_server_hello, unsigned int out_cap, unsigned int* out_len){
    if (!client_hello || !out_server_hello || !out_len){
        return -1;
    }
    sync_user_read(client_hello, client_hello_len);

    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = lookup_locked(pid, tls_id);
    if (si < 0 || g_tls_sessions[si].role != QOS_TLS_ROLE_SERVER){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    tls_session_t* s = &g_tls_sessions[si];

    unsigned char peer_pub[32];
    unsigned char sh[256];
    unsigned int sh_len = 0;
    if (tls13_process_client_hello_and_build_server_hello_x25519(client_hello, client_hello_len,
                                                                  peer_pub, s->local_public,
                                                                  sh, sizeof(sh), &sh_len) != 0){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    if (sh_len > out_cap){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    for (unsigned int i = 0; i < sh_len; i++){
        out_server_hello[i] = sh[i];
    }
    *out_len = sh_len;
    sync_user_write(out_server_hello, sh_len);

    for (unsigned int i = 0; i < 32u; i++){
        s->peer_public[i] = peer_pub[i];
    }
    s->has_peer = 1;

    unsigned char th[32];
    if (tls13_transcript_hash2(client_hello, client_hello_len, sh, sh_len, th) != 0){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -1;
    }
    int rc = derive_locked(s, th);
    spin_unlock_irqrestore(&g_tls_lock, irq);
    return rc;
}

int ktls_record_encrypt(int pid, int tls_id, qos_tls_record_io_t* io){
    if (!io){
        return -1;
    }
    sync_user_read(io, (unsigned int)sizeof(*io));
    if (!io->out){
        return -2;
    }
    if (io->in && io->in_len){
        sync_user_read(io->in, io->in_len);
    }
    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = lookup_locked(pid, tls_id);
    if (si < 0 || !g_tls_sessions[si].ready){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -3;
    }
    tls_session_t* s = &g_tls_sessions[si];
    unsigned int n = 0;
    int rc = tls13_record_encrypt(&s->tx,
                                  io->inner_type,
                                  io->in, io->in_len,
                                  io->out, io->out_cap, &n);
    io->out_len = n;
    spin_unlock_irqrestore(&g_tls_lock, irq);
    if (rc == 0){
        sync_user_write(io->out, n);
    }
    sync_user_write(io, (unsigned int)sizeof(*io));
    return rc == 0 ? (int)n : -4;
}

int ktls_record_decrypt(int pid, int tls_id, qos_tls_record_io_t* io){
    if (!io){
        return -1;
    }
    sync_user_read(io, (unsigned int)sizeof(*io));
    if (!io->in || !io->out){
        return -2;
    }
    sync_user_read(io->in, io->in_len);
    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = lookup_locked(pid, tls_id);
    if (si < 0 || !g_tls_sessions[si].ready){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return -3;
    }
    tls_session_t* s = &g_tls_sessions[si];
    unsigned int n = 0;
    unsigned char inner = 0;
    int rc = tls13_record_decrypt(&s->rx,
                                  io->in, io->in_len,
                                  io->out, io->out_cap, &n, &inner);
    io->out_len = n;
    io->inner_type = inner;
    spin_unlock_irqrestore(&g_tls_lock, irq);
    if (rc == 0){
        sync_user_write(io->out, n);
    }
    sync_user_write(io, (unsigned int)sizeof(*io));
    return rc == 0 ? (int)n : -4;
}

int ktls_is_ready(int pid, int tls_id){
    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int si = lookup_locked(pid, tls_id);
    if (si < 0){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        return 0;
    }
    int ready = g_tls_sessions[si].ready;
    spin_unlock_irqrestore(&g_tls_lock, irq);
    return ready;
}

int tls_session_self_test(void){
    enum { TEST_PID = 0 };
    int c = -1;
    int s = -1;
    static unsigned char ch[256];
    static unsigned char sh[256];
    unsigned int ch_len = 0;
    unsigned int sh_len = 0;
    static const unsigned char msg[] = "tls-session-self-test";
    static unsigned char c_ct[256];
    static unsigned char s_pt[256];
    unsigned int c_ct_len = 0;
    unsigned int s_pt_len = 0;
    unsigned char s_inner = 0;

    c = ktls_open(TEST_PID, QOS_TLS_ROLE_CLIENT);
    if (c < 0){
        return -1;
    }
    s = ktls_open(TEST_PID, QOS_TLS_ROLE_SERVER);
    if (s < 0){
        (void)ktls_close(TEST_PID, c);
        return -2;
    }
    if (ktls_build_client_hello(TEST_PID, c, ch, sizeof(ch), &ch_len) != 0 || ch_len == 0u){
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -3;
    }
    if (ktls_process_client_hello_build_server_hello(TEST_PID, s, ch, ch_len, sh, sizeof(sh), &sh_len) != 0 || sh_len == 0u){
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -4;
    }
    if (ktls_process_server_hello(TEST_PID, c, sh, sh_len) != 0){
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -5;
    }
    if (!ktls_is_ready(TEST_PID, c) || !ktls_is_ready(TEST_PID, s)){
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -6;
    }

    unsigned long irq = spin_lock_irqsave(&g_tls_lock);
    int ci = lookup_locked(TEST_PID, c);
    int si = lookup_locked(TEST_PID, s);
    if (ci < 0 || si < 0){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -9;
    }
    if (!crypto_consttime_equal(g_tls_sessions[ci].shared_secret, g_tls_sessions[si].shared_secret, 32u)){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -10;
    }
    if (!crypto_consttime_equal(g_tls_sessions[ci].hs.client_key, g_tls_sessions[si].hs.client_key, TLS13_KEY_BYTES)){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -11;
    }
    if (!crypto_consttime_equal(g_tls_sessions[ci].hs.client_iv, g_tls_sessions[si].hs.client_iv, TLS13_IV_BYTES)){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -12;
    }
    if (!crypto_consttime_equal(g_tls_sessions[ci].hs.server_key, g_tls_sessions[si].hs.server_key, TLS13_KEY_BYTES)){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -13;
    }
    if (!crypto_consttime_equal(g_tls_sessions[ci].hs.server_iv, g_tls_sessions[si].hs.server_iv, TLS13_IV_BYTES)){
        spin_unlock_irqrestore(&g_tls_lock, irq);
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -14;
    }
    spin_unlock_irqrestore(&g_tls_lock, irq);

    qos_tls_record_io_t ioe;
    ioe.inner_type = QOS_TLS_RECORD_INNER_APPDATA;
    ioe.in = msg;
    ioe.in_len = (unsigned int)(sizeof(msg) - 1u);
    ioe.out = c_ct;
    ioe.out_cap = sizeof(c_ct);
    ioe.out_len = 0;
    int enc = ktls_record_encrypt(TEST_PID, c, &ioe);
    c_ct_len = ioe.out_len;
    if (enc <= 0 || c_ct_len == 0u){
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -7;
    }

    qos_tls_record_io_t iod;
    iod.inner_type = 0;
    iod.in = c_ct;
    iod.in_len = c_ct_len;
    iod.out = s_pt;
    iod.out_cap = sizeof(s_pt);
    iod.out_len = 0;
    int dec = ktls_record_decrypt(TEST_PID, s, &iod);
    s_pt_len = iod.out_len;
    s_inner = iod.inner_type;
    if (dec <= 0 || s_inner != QOS_TLS_RECORD_INNER_APPDATA || s_pt_len != (unsigned int)(sizeof(msg) - 1u) ||
        !crypto_consttime_equal(s_pt, msg, s_pt_len)){
        (void)ktls_close(TEST_PID, s);
        (void)ktls_close(TEST_PID, c);
        return -8;
    }

    (void)ktls_close(TEST_PID, s);
    (void)ktls_close(TEST_PID, c);
    return 0;
}
