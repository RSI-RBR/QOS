#include "syscall.h"
#include "uart.h"
#include "process.h"
#include "framebuffer.h"
#include "timer.h"
#include "loader.h"
#include "memory.h"
#include "net.h"
#include "net_proto.h"
#include "udp.h"
#include "tcp.h"
#include "usb_host.h"
#include "interrupt.h"
#include "socket.h"
#include "console.h"
#include "tls_session.h"
#include "remote_login.h"
#include "cyw43.h"
#include "auth.h"
#include "trust.h"
#include "terminal.h"
#include "dma.h"
#include "klog.h"
#include "display.h"
#include "blockdev.h"
#include "fat32.h"
#include "mmu.h"
#include "mailbox.h"
#include "gpu2d.h"
#include "v3d.h"
#include "headless_control.h"
#include "sandbox_file.h"
#include "scanner_log.h"
#include "aes_gcm.h"
#include "crypto.h"
#include "spinlock.h"
#include "argon2_kdf.h"

#define ESR_EC_SHIFT 26
#define ESR_EC_MASK   0x3FUL
#define ESR_EC_SVC64  0x15UL
#define USER_CSTR_MAX 256u
#define USER_PASS_MAX 128u
#define USER_IO_MAX   16384u
#define USER_WIFI_SCAN_MAX 64u
#define USER_BMP_FILE_MAX (4u * 1024u * 1024u)
#define USER_BMP_PATH_MAX 96u
#define USER_FILE_RW_MAX (256u * 1024u)
#define SCANNER_LOG_ENC_MAGIC_LEN 8u
#define SCANNER_LOG_ENC_HDR_LEN 40u
static const unsigned char g_scanner_log_enc_magic[SCANNER_LOG_ENC_MAGIC_LEN] = {
    'Q','O','S','E','N','C','1','\n'
};
static spinlock_t g_scanner_log_key_lock = {0};
static unsigned char g_scanner_log_key[32];
static unsigned int g_scanner_log_key_valid = 0u;

static int validate_gpu2d_blit_source(const qos_gpu2d_blit_t* blit){
    if (!blit || !blit->pixels || blit->texture_w == 0u || blit->texture_h == 0u ||
        blit->src_w == 0u || blit->src_h == 0u ||
        blit->texture_w > 0x3FFFFFFFu ||
        blit->pitch < blit->texture_w * 4u ||
        blit->src_x + blit->src_w < blit->src_x ||
        blit->src_y + blit->src_h < blit->src_y ||
        blit->src_x + blit->src_w > blit->texture_w ||
        blit->src_y + blit->src_h > blit->texture_h){
        return -1;
    }
    unsigned long first = ((unsigned long)blit->src_y * (unsigned long)blit->pitch) +
                          ((unsigned long)blit->src_x * 4ul);
    unsigned long last = ((unsigned long)(blit->src_y + blit->src_h - 1u) *
                         (unsigned long)blit->pitch) +
                         ((unsigned long)(blit->src_x + blit->src_w) * 4ul);
    unsigned long base = (unsigned long)blit->pixels;
    if (last < first || (~0ul - base) < first){
        return -1;
    }
    return process_user_range_readable((const void*)(base + first), last - first) ? 0 : -1;
}

// Trap frame layout in vectors.S
#define TF_X0   0
#define TF_X1   1
#define TF_X2   2
#define TF_X3   3
#define TF_X4   4
#define TF_X8   8

static unsigned long clamp_puts_len(const char* s){
    unsigned long max = 1024;
    unsigned long n = 0;
    while (n < max && s[n]){
        n++;
    }
    return n;
}

static int scanner_fat_prepare_emmc(void){
    if (cyw43_monitor_capture_active()){
        /*
         * Pi 3/Zero-class WiFi shares the EMMC/SDIO controller with storage.
         * Reclaiming it while monitor capture is alive leaves Nexmon in a
         * half-up state on real boards. Refuse FAT log I/O until WiFi/monitor
         * has been explicitly stopped.
         */
        uart_puts("Scanner FAT: WiFi owns EMMC; stop scanner/monitor before FAT log I/O\n");
        return -1;
    }
    if (blockdev_is_emmc()){
        fat32_reset();
        return 0;
    }
    if (blockdev_reinit_emmc_from_wifi() == 0 && blockdev_is_emmc()){
        fat32_reset();
        return 0;
    }

    /*
     * SDHOST has been too flaky for long-running scanner persistence. Keep the
     * RAM log intact and retry on the next autosave instead of risking FAT
     * writes through SDHOST.
     */
    uart_puts("Scanner FAT: EMMC unavailable; SDHOST save disabled\n");
    return -1;
}

static int cstr_eq_lit(const char* a, const char* b){
    if (!a || !b){
        return 0;
    }
    while (*a && *b){
        if (*a != *b){
            return 0;
        }
        a++;
        b++;
    }
    return (*a == 0 && *b == 0) ? 1 : 0;
}

static int scanner_log_is_replace_file(const char* path){
    return (cstr_eq_lit(path, "counts.log") || cstr_eq_lit(path, "aps.log")) ? 1 : 0;
}

static void scanner_log_store_be32(unsigned char out[4], unsigned int v){
    out[0] = (unsigned char)((v >> 24) & 0xFFu);
    out[1] = (unsigned char)((v >> 16) & 0xFFu);
    out[2] = (unsigned char)((v >> 8) & 0xFFu);
    out[3] = (unsigned char)(v & 0xFFu);
}

static unsigned int scanner_log_load_be32(const unsigned char in[4]){
    return ((unsigned int)in[0] << 24) |
           ((unsigned int)in[1] << 16) |
           ((unsigned int)in[2] << 8) |
           (unsigned int)in[3];
}

static int scanner_log_is_encrypted_buf(const unsigned char* data, unsigned int len){
    if (!data || len < SCANNER_LOG_ENC_MAGIC_LEN){
        return 0;
    }
    for (unsigned int i = 0u; i < SCANNER_LOG_ENC_MAGIC_LEN; i++){
        if (data[i] != g_scanner_log_enc_magic[i]){
            return 0;
        }
    }
    return 1;
}

static void scanner_log_clear_password(void){
    unsigned long irq = spin_lock_irqsave(&g_scanner_log_key_lock);
    crypto_memzero(g_scanner_log_key, sizeof(g_scanner_log_key));
    g_scanner_log_key_valid = 0u;
    spin_unlock_irqrestore(&g_scanner_log_key_lock, irq);
}

static int scanner_log_copy_key(unsigned char out[32]){
    unsigned long irq;
    if (!out){
        return -1;
    }
    irq = spin_lock_irqsave(&g_scanner_log_key_lock);
    if (!g_scanner_log_key_valid){
        spin_unlock_irqrestore(&g_scanner_log_key_lock, irq);
        return -1;
    }
    for (unsigned int i = 0u; i < 32u; i++){
        out[i] = g_scanner_log_key[i];
    }
    spin_unlock_irqrestore(&g_scanner_log_key_lock, irq);
    return 0;
}

static int scanner_log_has_key(void){
    unsigned int valid;
    unsigned long irq = spin_lock_irqsave(&g_scanner_log_key_lock);
    valid = g_scanner_log_key_valid;
    spin_unlock_irqrestore(&g_scanner_log_key_lock, irq);
    return valid ? 1 : 0;
}

static int scanner_log_set_password_kernel(const char* password){
    static const unsigned char salt[] = "QOS scanner log password v1";
    unsigned char new_key[32];
    unsigned long irq;
    int kdf_rc;
    unsigned int pw_len = 0u;

    if (!password || !password[0]){
        scanner_log_clear_password();
        return -1;
    }
    while (password[pw_len] && pw_len < USER_PASS_MAX - 1u){
        pw_len++;
    }
    if (pw_len == 0u){
        scanner_log_clear_password();
        return -1;
    }

    kdf_rc = argon2id_hash_raw_qos(AUTH_ARGON2_DEFAULT_T_COST,
                                   AUTH_ARGON2_DEFAULT_M_COST_KIB,
                                   AUTH_ARGON2_DEFAULT_PARALLELISM,
                                   (const void*)password,
                                   pw_len,
                                   salt,
                                   (unsigned int)(sizeof(salt) - 1u),
                                   new_key,
                                   sizeof(new_key),
                                   AUTH_ARGON2_DEFAULT_VERSION);
    if (kdf_rc != 0){
        crypto_memzero(new_key, sizeof(new_key));
        scanner_log_clear_password();
        return -1;
    }
    irq = spin_lock_irqsave(&g_scanner_log_key_lock);
    for (unsigned int i = 0u; i < 32u; i++){
        g_scanner_log_key[i] = new_key[i];
    }
    g_scanner_log_key_valid = 1u;
    spin_unlock_irqrestore(&g_scanner_log_key_lock, irq);
    crypto_memzero(new_key, sizeof(new_key));
    return 0;
}

static int scanner_log_encrypt_record(const unsigned char* plain,
                                      unsigned int plain_len,
                                      unsigned char** out_record,
                                      unsigned int* out_len){
    aes_gcm_key_t key;
    unsigned char key_bytes[32];
    unsigned char* rec;
    unsigned int rec_len;

    if (!out_record || !out_len || !plain || plain_len == 0u ||
        plain_len > USER_FILE_RW_MAX - SCANNER_LOG_ENC_HDR_LEN ||
        scanner_log_copy_key(key_bytes) != 0){
        return -1;
    }
    rec_len = SCANNER_LOG_ENC_HDR_LEN + plain_len;
    rec = (unsigned char*)kmalloc(rec_len);
    if (!rec){
        crypto_memzero(key_bytes, sizeof(key_bytes));
        return -1;
    }
    for (unsigned int i = 0u; i < SCANNER_LOG_ENC_MAGIC_LEN; i++){
        rec[i] = g_scanner_log_enc_magic[i];
    }
    scanner_log_store_be32(&rec[8], plain_len);
    if (crypto_random_bytes(&rec[12], 12u) != 0){
        crypto_memzero(key_bytes, sizeof(key_bytes));
        kfree_secure(rec, rec_len);
        return -1;
    }
    for (unsigned int i = 24u; i < 40u; i++){
        rec[i] = 0u;
    }
    if (aes_gcm_key_init(&key, key_bytes, sizeof(key_bytes)) != 0){
        crypto_memzero(key_bytes, sizeof(key_bytes));
        kfree_secure(rec, rec_len);
        return -1;
    }
    crypto_memzero(key_bytes, sizeof(key_bytes));
    if (aes_gcm_encrypt(&key,
                        &rec[12], 12u,
                        rec, 24u,
                        plain, plain_len,
                        &rec[SCANNER_LOG_ENC_HDR_LEN],
                        &rec[24], 16u) != 0){
        crypto_memzero(&key, sizeof(key));
        kfree_secure(rec, rec_len);
        return -1;
    }
    crypto_memzero(&key, sizeof(key));
    *out_record = rec;
    *out_len = rec_len;
    return 0;
}

static int scanner_log_decrypt_records(const unsigned char* enc,
                                       unsigned int enc_len,
                                       unsigned char* out,
                                       unsigned int out_cap){
    aes_gcm_key_t key;
    unsigned char key_bytes[32];
    unsigned int pos = 0u;
    unsigned int out_len = 0u;

    if (!enc || !out || out_cap == 0u){
        return -1;
    }
    if (!scanner_log_is_encrypted_buf(enc, enc_len)){
        unsigned int n = enc_len;
        if (n > out_cap){
            n = out_cap;
        }
        for (unsigned int i = 0u; i < n; i++){
            out[i] = enc[i];
        }
        return (int)n;
    }
    if (scanner_log_copy_key(key_bytes) != 0){
        return -1;
    }
    if (aes_gcm_key_init(&key, key_bytes, sizeof(key_bytes)) != 0){
        crypto_memzero(key_bytes, sizeof(key_bytes));
        return -1;
    }
    crypto_memzero(key_bytes, sizeof(key_bytes));
    while (pos < enc_len){
        unsigned int plain_len;
        if (pos + SCANNER_LOG_ENC_HDR_LEN > enc_len ||
            !scanner_log_is_encrypted_buf(&enc[pos], enc_len - pos)){
            crypto_memzero(&key, sizeof(key));
            return -1;
        }
        plain_len = scanner_log_load_be32(&enc[pos + 8u]);
        if (plain_len > USER_FILE_RW_MAX ||
            pos + SCANNER_LOG_ENC_HDR_LEN + plain_len > enc_len ||
            out_len + plain_len > out_cap){
            crypto_memzero(&key, sizeof(key));
            return -1;
        }
        if (aes_gcm_decrypt(&key,
                            &enc[pos + 12u], 12u,
                            &enc[pos], 24u,
                            &enc[pos + SCANNER_LOG_ENC_HDR_LEN],
                            plain_len,
                            &out[out_len],
                            &enc[pos + 24u], 16u) != 0){
            crypto_memzero(&key, sizeof(key));
            return -1;
        }
        out_len += plain_len;
        pos += SCANNER_LOG_ENC_HDR_LEN + plain_len;
    }
    crypto_memzero(&key, sizeof(key));
    return (int)out_len;
}

static int scanner_log_flush_to_fat_encrypted(const char scanner83[11],
                                              const char* path,
                                              int replace_file){
    unsigned char* plain = 0;
    unsigned char* rec = 0;
    unsigned char* old = 0;
    unsigned char* old_rec = 0;
    unsigned int rec_len = 0u;
    unsigned int old_rec_len = 0u;
    int size;
    int rc;
    int old_n;

    if (!scanner_log_has_key()){
        return -1;
    }

    size = sandbox_file_size(scanner83, path);
    if (size <= 0){
        return (size == 0) ? 0 : -1;
    }
    if ((unsigned int)size > USER_FILE_RW_MAX - SCANNER_LOG_ENC_HDR_LEN){
        return -1;
    }
    plain = (unsigned char*)kmalloc((unsigned long)size);
    if (!plain){
        return -1;
    }
    if (sandbox_file_read(scanner83, path, plain, (unsigned int)size) != size){
        kfree_secure(plain, (unsigned int)size);
        return -1;
    }
    if (scanner_log_encrypt_record(plain, (unsigned int)size, &rec, &rec_len) != 0){
        kfree_secure(plain, (unsigned int)size);
        return -1;
    }
    if (replace_file){
        rc = fat32_write_file_in_dir_path_existing(scanner83, path, rec, rec_len);
    } else{
        /*
         * First encrypted run after old plaintext captures: rewrite the
         * existing plaintext log as one encrypted record before appending the
         * new record. Otherwise handshakes.log would become mixed text/binary.
         */
        old = (unsigned char*)kmalloc(USER_FILE_RW_MAX);
        if (!old){
            kfree_secure(plain, (unsigned int)size);
            kfree_secure(rec, rec_len);
            return -1;
        }
        old_n = fat32_read_file_in_dir_path_any(scanner83, path, old, USER_FILE_RW_MAX);
        if (old_n > 0 && !scanner_log_is_encrypted_buf(old, (unsigned int)old_n)){
            if ((unsigned int)old_n > USER_FILE_RW_MAX - SCANNER_LOG_ENC_HDR_LEN ||
                scanner_log_encrypt_record(old, (unsigned int)old_n, &old_rec, &old_rec_len) != 0){
                kfree_secure(old, USER_FILE_RW_MAX);
                kfree_secure(plain, (unsigned int)size);
                kfree_secure(rec, rec_len);
                return -1;
            }
            rc = fat32_write_file_in_dir_path_existing(scanner83, path, old_rec, old_rec_len);
            if (rc == 0){
                rc = fat32_append_file_in_dir_path_existing(scanner83, path, rec, rec_len);
            }
        } else{
            rc = fat32_append_file_in_dir_path_existing(scanner83, path, rec, rec_len);
        }
    }
    if (rc == 0){
        (void)sandbox_file_clear(scanner83, path);
    }
    if (old){
        kfree_secure(old, USER_FILE_RW_MAX);
    }
    if (old_rec){
        kfree_secure(old_rec, old_rec_len);
    }
    kfree_secure(plain, (unsigned int)size);
    kfree_secure(rec, rec_len);
    return rc;
}

static int scanner_log_flush_all_files_encrypted(void){
    static const char scanner83[11] = {'S','C','A','N','N','E','R',' ',' ',' ',' '};
    int rc_counts = scanner_log_flush_to_fat_encrypted(scanner83, "counts.log", 1);
    int rc_aps = scanner_log_flush_to_fat_encrypted(scanner83, "aps.log", 1);
    int rc_hs = scanner_log_flush_to_fat_encrypted(scanner83, "handshakes.log", 0);
    return (rc_counts == 0 && rc_aps == 0 && rc_hs == 0) ? 0 : -1;
}

int scanner_log_flush_all_to_fat_kernel(void){
    int rc = -1;

    kernel_preempt_enter();
    if (scanner_fat_prepare_emmc() == 0 && fat32_init() == 0){
        rc = scanner_log_flush_all_files_encrypted();
    }
    if (rc != 0 && blockdev_reinit_emmc_from_wifi() == 0 &&
        blockdev_is_emmc() && fat32_init() == 0){
        rc = scanner_log_flush_all_files_encrypted();
    }
    kernel_preempt_exit();
    return rc;
}

static int scanner_log_read_fat_plaintext(const char scanner83[11],
                                          const char* path,
                                          unsigned int offset,
                                          unsigned char* out,
                                          unsigned int out_cap){
    unsigned char* enc = 0;
    unsigned char* plain = 0;
    int enc_n;
    int plain_n;
    int rc = -1;

    if (!out || out_cap == 0u){
        return -1;
    }
    enc = (unsigned char*)kmalloc(USER_FILE_RW_MAX);
    plain = (unsigned char*)kmalloc(USER_FILE_RW_MAX);
    if (!enc || !plain){
        goto out;
    }
    enc_n = fat32_read_file_in_dir_path_any(scanner83, path, enc, USER_FILE_RW_MAX);
    if (enc_n < 0){
        goto out;
    }
    if (enc_n == 0){
        rc = 0;
        goto out;
    }
    plain_n = scanner_log_decrypt_records(enc, (unsigned int)enc_n, plain, USER_FILE_RW_MAX);
    if (plain_n < 0){
        goto out;
    }
    if (offset >= (unsigned int)plain_n){
        rc = 0;
        goto out;
    }
    rc = (int)((unsigned int)plain_n - offset);
    if ((unsigned int)rc > out_cap){
        rc = (int)out_cap;
    }
    for (unsigned int i = 0u; i < (unsigned int)rc; i++){
        out[i] = plain[offset + i];
    }

out:
    if (enc){
        kfree_secure(enc, USER_FILE_RW_MAX);
    }
    if (plain){
        kfree_secure(plain, USER_FILE_RW_MAX);
    }
    return rc;
}

static int copy_cstr_out(char* out, unsigned int out_cap, const char* in){
    unsigned int i = 0;
    if (!out || out_cap == 0u || !in){
        return -1;
    }
    while (i + 1u < out_cap && in[i]){
        out[i] = in[i];
        i++;
    }
    out[i] = 0;
    return 0;
}

static unsigned int clamp_u32(unsigned int v, unsigned int max){
    return (v > max) ? max : v;
}

static int copy_cstr_from_user_bound(char* out, unsigned int out_cap, const char* user_in){
    return process_copy_cstr_from_user(out, out_cap, user_in);
}

static unsigned int cstr_bytes_with_nul(const char* s, unsigned int cap){
    unsigned int n = 0;
    if (!s || cap == 0u){
        return 0u;
    }
    while (n + 1u < cap && s[n]){
        n++;
    }
    return n + 1u;
}

static unsigned long counter_cycles_to_us(unsigned long cycles, unsigned long hz){
    if (hz == 0ul){
        return 0ul;
    }
    {
        // Keep this 64-bit only in freestanding builds to avoid compiler
        // runtime helpers like __udivti3 from 128-bit division.
        unsigned long whole = (unsigned long)(((unsigned long long)(cycles / hz)) * 1000000ull);
        unsigned long rem = cycles % hz;
        return whole + (unsigned long)(((unsigned long long)rem * 1000000ull) / hz);
    }
}

static unsigned long counter_cycles_to_ns(unsigned long cycles, unsigned long hz){
    if (hz == 0ul){
        return 0ul;
    }
    {
        // Keep this 64-bit only in freestanding builds to avoid compiler
        // runtime helpers like __udivti3 from 128-bit division.
        unsigned long whole = (unsigned long)(((unsigned long long)(cycles / hz)) * 1000000000ull);
        unsigned long rem = cycles % hz;
        return whole + (unsigned long)(((unsigned long long)rem * 1000000000ull) / hz);
    }
}

typedef struct file_profile_stats {
    unsigned long calls;
    unsigned long ok;
    unsigned long fail;
    unsigned long bytes;
    unsigned long cstr_us;
    unsigned long alloc_us;
    unsigned long fat_init_us;
    unsigned long fat_read_us;
    unsigned long copy_us;
    unsigned long total_us;
    unsigned long max_total_us;
} file_profile_stats_t;

static file_profile_stats_t g_file_profile;

static unsigned long profile_time_us(void){
    unsigned long cycles;
    unsigned long hz;
    asm volatile("mrs %0, cntpct_el0" : "=r"(cycles));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return counter_cycles_to_us(cycles, hz);
}

static void file_profile_reset(void){
    g_file_profile.calls = 0;
    g_file_profile.ok = 0;
    g_file_profile.fail = 0;
    g_file_profile.bytes = 0;
    g_file_profile.cstr_us = 0;
    g_file_profile.alloc_us = 0;
    g_file_profile.fat_init_us = 0;
    g_file_profile.fat_read_us = 0;
    g_file_profile.copy_us = 0;
    g_file_profile.total_us = 0;
    g_file_profile.max_total_us = 0;
}

static void file_profile_note(int ok,
                              unsigned long bytes,
                              unsigned long cstr_us,
                              unsigned long alloc_us,
                              unsigned long fat_init_us,
                              unsigned long fat_read_us,
                              unsigned long copy_us,
                              unsigned long total_us){
    g_file_profile.calls++;
    if (ok){
        g_file_profile.ok++;
        g_file_profile.bytes += bytes;
    } else{
        g_file_profile.fail++;
    }
    g_file_profile.cstr_us += cstr_us;
    g_file_profile.alloc_us += alloc_us;
    g_file_profile.fat_init_us += fat_init_us;
    g_file_profile.fat_read_us += fat_read_us;
    g_file_profile.copy_us += copy_us;
    g_file_profile.total_us += total_us;
    if (total_us > g_file_profile.max_total_us){
        g_file_profile.max_total_us = total_us;
    }
}

static void file_profile_dump(void){
    uart_puts("FILE BMP profile: calls=");
    uart_putdec(g_file_profile.calls);
    uart_puts(" ok=");
    uart_putdec(g_file_profile.ok);
    uart_puts(" fail=");
    uart_putdec(g_file_profile.fail);
    uart_puts(" bytes=");
    uart_putdec(g_file_profile.bytes);
    uart_puts("\n");

    uart_puts("FILE BMP us: cstr=");
    uart_putdec(g_file_profile.cstr_us);
    uart_puts(" alloc=");
    uart_putdec(g_file_profile.alloc_us);
    uart_puts(" fat_init=");
    uart_putdec(g_file_profile.fat_init_us);
    uart_puts(" fat_read=");
    uart_putdec(g_file_profile.fat_read_us);
    uart_puts(" copy=");
    uart_putdec(g_file_profile.copy_us);
    uart_puts(" total=");
    uart_putdec(g_file_profile.total_us);
    uart_puts(" max=");
    uart_putdec(g_file_profile.max_total_us);
    uart_puts("\n");
}

static void syscall_poll_background_io(void){
    static unsigned long next_net_poll_tick = 0;
    static unsigned long next_remote_poll_tick = 0;
    static unsigned long next_wifi_raw_poll_tick = 0;
    unsigned long now = system_ticks;

    // HID is collected by a background pump; consumers drain queued input so
    // no-data USB polls do not block graphics/event syscalls.
    usb_host_service();
    terminal_poll_inputs();

    if ((long)(now - next_net_poll_tick) >= 0){
        // The default NIC path is USB-backed on Pi 3, so background polling is
        // intentionally modest. Explicit network syscalls still poll directly.
        next_net_poll_tick = now + 50u;
        (void)net_poll();
    }
    if ((long)(now - next_wifi_raw_poll_tick) >= 0){
        next_wifi_raw_poll_tick = now + 10u;
        if (cyw43_raw_capture_is_enabled()){
            (void)cyw43_raw_capture_poll_lite();
        }
    }
    if ((long)(now - next_remote_poll_tick) >= 0){
        next_remote_poll_tick = now + 10u;
        remote_login_poll();
    }
    headless_control_poll();
}

static void syscall_write_puts(int pid, const char* s){
    if (!s){
        return;
    }
    unsigned long n = clamp_puts_len(s);
    terminal_write_for_pid(pid, s, n);
}

static void syscall_dump_usb_info(void){
    usb_root_device_info_t info;
    if (usb_host_get_root_device_info(&info) != 0){
        syscall_write_puts(-1, "USB root: not enumerated\n");
        return;
    }

    syscall_write_puts(-1, "USB root addr=");
    uart_puthex(info.address);
    syscall_write_puts(-1, " vid=");
    uart_puthex(info.vid);
    syscall_write_puts(-1, " pid=");
    uart_puthex(info.pid);
    syscall_write_puts(-1, " class=");
    uart_puthex(info.dev_class);
    syscall_write_puts(-1, " cfg=");
    uart_puthex(info.config_value);
    syscall_write_puts(-1, info.configured ? " (set)\n" : " (not set)\n");

    if (info.child_present){
        syscall_write_puts(-1, "USB child addr=");
        uart_puthex(info.child_address);
        syscall_write_puts(-1, " vid=");
        uart_puthex(info.child_vid);
        syscall_write_puts(-1, " pid=");
        uart_puthex(info.child_pid);
        syscall_write_puts(-1, " class=");
        uart_puthex(info.child_class);
        syscall_write_puts(-1, " cfg=");
        uart_puthex(info.child_config_value);
        syscall_write_puts(-1, info.child_configured ? " (set)\n" : " (not set)\n");
        syscall_write_puts(-1, "USB child bulk in=");
        uart_puthex(info.child_bulk_in_ep);
        syscall_write_puts(-1, " mps=");
        uart_puthex(info.child_bulk_in_mps);
        syscall_write_puts(-1, " out=");
        uart_puthex(info.child_bulk_out_ep);
        syscall_write_puts(-1, " mps=");
        uart_puthex(info.child_bulk_out_mps);
        syscall_write_puts(-1, "\n");
    }
    syscall_write_puts(-1, "USB hub ports=");
    uart_putdec(info.hub_ports);
    syscall_write_puts(-1, " conn_mask=");
    uart_puthex(info.hub_connected_mask);
    syscall_write_puts(-1, " enum_attempts=");
    uart_putdec(info.hub_enum_attempts);
    syscall_write_puts(-1, " enum_ok=");
    uart_putdec(info.hub_enum_success);
    syscall_write_puts(-1, " enum_ok_mask=");
    uart_puthex(info.hub_enum_success_mask);
    syscall_write_puts(-1, " hid_candidates=");
    uart_putdec(info.hub_hid_candidates);
    syscall_write_puts(-1, " hid_mask=");
    uart_puthex(info.hub_hid_candidate_mask);
    syscall_write_puts(-1, "\n");
    for (unsigned int port = 1; port < USB_HOST_MAX_TRACKED_PORTS; port++){
        if (info.port_addr[port] == 0u){
            continue;
        }
        syscall_write_puts(-1, "USB p");
        uart_putdec(port);
        syscall_write_puts(-1, " addr=");
        uart_puthex(info.port_addr[port]);
        syscall_write_puts(-1, " vid=");
        uart_puthex(info.port_vid[port]);
        syscall_write_puts(-1, " pid=");
        uart_puthex(info.port_pid[port]);
        syscall_write_puts(-1, " class=");
        uart_puthex(info.port_class[port]);
        syscall_write_puts(-1, " cfg=");
        uart_puthex(info.port_config[port]);
        syscall_write_puts(-1, " intr=");
        uart_puthex(info.port_intr_in_ep[port]);
        syscall_write_puts(-1, " mps=");
        uart_puthex(info.port_intr_in_mps[port]);
        syscall_write_puts(-1, "\n");
    }

    if (info.child_hid_kbd_present){
        syscall_write_puts(-1, "USB HID kbd addr=");
        uart_puthex(info.child_hid_kbd_address);
        syscall_write_puts(-1, " iface=");
        uart_puthex(info.child_hid_kbd_iface);
        syscall_write_puts(-1, " ep=");
        uart_puthex(info.child_hid_kbd_ep);
        syscall_write_puts(-1, " mps=");
        uart_puthex(info.child_hid_kbd_mps);
        syscall_write_puts(-1, "\n");
        syscall_write_puts(-1, "USB HID poll=");
        uart_putdec(info.hid_poll_count);
        syscall_write_puts(-1, " reports=");
        uart_putdec(info.hid_report_count);
        syscall_write_puts(-1, " nodata=");
        uart_putdec(info.hid_nodata_count);
        syscall_write_puts(-1, " errors=");
        uart_putdec(info.hid_error_count);
        syscall_write_puts(-1, " last=");
        uart_putdec(info.hid_last_actual);
        syscall_write_puts(-1, " off=");
        uart_puthex(info.hid_report_offset);
        syscall_write_puts(-1, " mod=");
        uart_puthex(info.hid_last_mod);
        syscall_write_puts(-1, " key=");
        uart_puthex(info.hid_last_key0);
        syscall_write_puts(-1, " ascii=");
        uart_puthex(info.hid_last_ascii);
        syscall_write_puts(-1, " down=");
        uart_puthex(info.hid_last_down_key);
        syscall_write_puts(-1, " q=");
        uart_putdec(info.hid_char_queue_count);
        syscall_write_puts(-1, " evq=");
        uart_putdec(info.hid_event_queue_count);
        syscall_write_puts(-1, " switches=");
        uart_putdec(info.hid_switch_count);
        syscall_write_puts(-1, " stale_clear=");
        uart_putdec(info.hid_stale_clear_count);
        syscall_write_puts(-1, "\n");
        syscall_write_puts(-1, "USB HID raw0=");
        uart_puthex(info.hid_last_raw0);
        syscall_write_puts(-1, " raw1=");
        uart_puthex(info.hid_last_raw1);
        syscall_write_puts(-1, "\n");
    } else if (info.child_hid_kbd_address != 0u){
        syscall_write_puts(-1, "USB HID kbd candidate addr=");
        uart_puthex(info.child_hid_kbd_address);
        syscall_write_puts(-1, " iface=");
        uart_puthex(info.child_hid_kbd_iface);
        syscall_write_puts(-1, " ep=");
        uart_puthex(info.child_hid_kbd_ep);
        syscall_write_puts(-1, " mps=");
        uart_puthex(info.child_hid_kbd_mps);
        syscall_write_puts(-1, " (not active)\n");
    } else{
        syscall_write_puts(-1, "USB HID kbd: none\n");
    }

    if (info.child_hid_mouse_present){
        syscall_write_puts(-1, "USB HID mouse addr=");
        uart_puthex(info.child_hid_mouse_address);
        syscall_write_puts(-1, " iface=");
        uart_puthex(info.child_hid_mouse_iface);
        syscall_write_puts(-1, " ep=");
        uart_puthex(info.child_hid_mouse_ep);
        syscall_write_puts(-1, " mps=");
        uart_puthex(info.child_hid_mouse_mps);
        syscall_write_puts(-1, "\n");
    } else if (info.child_hid_mouse_address != 0u){
        syscall_write_puts(-1, "USB HID mouse candidate addr=");
        uart_puthex(info.child_hid_mouse_address);
        syscall_write_puts(-1, " iface=");
        uart_puthex(info.child_hid_mouse_iface);
        syscall_write_puts(-1, " ep=");
        uart_puthex(info.child_hid_mouse_ep);
        syscall_write_puts(-1, " mps=");
        uart_puthex(info.child_hid_mouse_mps);
        syscall_write_puts(-1, " (not active)\n");
    } else{
        syscall_write_puts(-1, "USB HID mouse: none\n");
    }
}

static int syscall_capability_allowed(const process_t* proc, unsigned long nr){
    unsigned int req_scope_any = 0u;
    unsigned int req_role_any = TRUST_ROLE_DEVELOPER | TRUST_ROLE_ADMIN;

    if (!proc || !proc->user_mode){
        return 1;
    }

    switch (nr){
        case SYS_PUTC:
        case SYS_PUTS:
        case SYS_SLEEP:
        case SYS_EXIT:
        case SYS_FB_CLEAR:
        case SYS_FB_GET_WIDTH:
        case SYS_FB_GET_HEIGHT:
        case SYS_FB_RECT:
        case SYS_FB_PRESENT:
        case SYS_FB_BLIT_RGBA:
        case SYS_FB_BLIT_NATIVE:
        case SYS_FB_ATTACH_BUFFER:
        case SYS_FB_DIRECT_ACQUIRE:
        case SYS_FB_DIRECT_PRESENT:
        case SYS_FB_DIRECT_GET_DRAW:
        case SYS_GPU2D_STATUS:
        case SYS_GPU2D_BLIT_RGBA:
        case SYS_GPU2D_BLIT_COUNT:
        case SYS_GPU2D_FALLBACK_COUNT:
        case SYS_GPU2D_UNSUPPORTED_COUNT:
        case SYS_GPU2D_CLEAR:
        case SYS_GPU2D_CLEAR_COUNT:
        case SYS_GPU2D_FILL_RECT:
        case SYS_GPU2D_FILL_COUNT:
        case SYS_GPU2D_QUAD_BATCH:
        case SYS_GPU2D_QUAD_COUNT:
        case SYS_GPU2D_QUAD_BATCH_COUNT:
        case SYS_GPU2D_QPU_STATUS:
        case SYS_GPU2D_QPU_QUAD_COUNT:
        case SYS_GPU2D_QPU_FAIL_COUNT:
        case SYS_GPU2D_V3D_STATUS:
        case SYS_GPU2D_V3D_QUAD_COUNT:
        case SYS_GPU2D_V3D_BATCH_COUNT:
        case SYS_GPU2D_V3D_FAIL_COUNT:
        case SYS_GPU2D_TEXTURE_UPLOAD:
        case SYS_GPU2D_TEXTURE_FREE:
        case SYS_GPU2D_TEXTURE_COUNT:
        case SYS_GPU2D_TEXTURE_UPLOAD_COUNT:
        case SYS_GPU2D_TEXTURE_FREE_COUNT:
        case SYS_GPU2D_TEXTURE_BYTES:
        case SYS_V3D_STATUS:
        case SYS_TRY_GETC:
        case SYS_TRY_GETC_EX:
        case SYS_INPUT_POLL_EVENT:
        case SYS_FILE_READ_BMP:
        case SYS_FILE_APPEND_DATA:
        case SYS_FILE_READ_DATA:
        case SYS_FILE_SIZE:
        case SYS_FILE_CLEAR:
        case SYS_FILE_PROFILE_RESET:
        case SYS_FILE_PROFILE_DUMP:
        case SYS_HEADLESS_OPEN_HIT:
        case SYS_GETPID:
        case SYS_GET_TICKS:
        case SYS_GET_COUNTER_HZ:
        case SYS_GET_COUNTER_CYCLES:
        case SYS_GET_TIME_US:
        case SYS_GET_TIME_NS:
            req_scope_any = TRUST_SCOPE_USER_APP | TRUST_SCOPE_SHELL | TRUST_SCOPE_WEB;
            break;

        case SYS_NET_DUMP_STATS:
        case SYS_NET_SEND_TEST_FRAME:
        case SYS_NET_POLL:
        case SYS_NET_RECV_RAW:
        case SYS_NET_SEND_RAW:
        case SYS_NET_PING_GATEWAY:
        case SYS_NET_UDP_SEND_PROBE:
        case SYS_NET_UDP_RECV:
        case SYS_NET_UDP_SEND:
        case SYS_NET_TCP_HTTP_GET:
        case SYS_SOCKET_CREATE:
        case SYS_SOCKET_CONNECT:
        case SYS_SOCKET_SEND:
        case SYS_SOCKET_RECV:
        case SYS_SOCKET_CLOSE:
        case SYS_SOCKET_SETOPT:
        case SYS_TLS_OPEN:
        case SYS_TLS_CLOSE:
        case SYS_TLS_GET_LOCAL_PUBLIC:
        case SYS_TLS_SET_PEER_PUBLIC:
        case SYS_TLS_BUILD_CLIENT_HELLO:
        case SYS_TLS_PROCESS_SERVER_HELLO:
        case SYS_TLS_PROCESS_CLIENT_HELLO_BUILD_SERVER_HELLO:
        case SYS_TLS_RECORD_ENCRYPT:
        case SYS_TLS_RECORD_DECRYPT:
        case SYS_TLS_IS_READY:
        case SYS_NET_GET_LOCAL_IP:
        case SYS_NET_GET_GATEWAY_IP:
            req_scope_any = TRUST_SCOPE_WEB | TRUST_SCOPE_SHELL;
            break;

        case SYS_RUN_PROGRAM:
        case SYS_RUN_PROGRAM_NAMED:
        case SYS_DISPLAY_CREATE_GRAPHICS:
        case SYS_DISPLAY_SWITCH_GRAPHICS:
        case SYS_DISPLAY_SWITCH_SESSION:
        case SYS_PROCESS_KILL:
        case SYS_PROCESS_STATE:
        case SYS_PROCESS_LOG_READ:
        case SYS_TTY_SET_OWNER:
        case SYS_TTY_RELEASE:
        case SYS_TTY_GET_OWNER:
        case SYS_TTY_CLAIM_SELF:
        case SYS_TERM_GET_ACTIVE:
        case SYS_TERM_SWITCH:
        case SYS_TERM_CLEAR:
        case SYS_TERM_GET_OUTPUT:
        case SYS_TERM_SET_OUTPUT:
        case SYS_TERM_SET_INPUT_LINE:
        case SYS_TERM_CLEAR_INPUT_LINE:
        case SYS_DMA_SET_ENABLED:
        case SYS_DMA_STATUS:
        case SYS_DMA_LAST_CS:
        case SYS_DMA_LAST_DEBUG:
        case SYS_DMA_TRANSFER_COUNT:
        case SYS_DMA_LAST_BYTES:
        case SYS_DMA_LAST_CLEAN_US:
        case SYS_DMA_LAST_WAIT_US:
        case SYS_DMA_LAST_TOTAL_US:
        case SYS_GPU_SET_ENABLED:
        case SYS_GPU_STATUS:
        case SYS_GPU_FLIP_COUNT:
        case SYS_SYSTEM_STATUS:
        case SYS_SYSTEM_SET_CLOCK:
        case SYS_V3D_PROBE:
        case SYS_V3D_NOOP:
        case SYS_V3D_CLEAR:
        case SYS_V3D_QPU_PROBE:
        case SYS_V3D_QPU_WRITE_PROBE:
        case SYS_V3D_QPU_EXEC_PROBE:
        case SYS_GPU2D_QPU_SET_ENABLED:
        case SYS_GPU2D_V3D_SET_ENABLED:
        case SYS_DISPLAY_PROFILE_RESET:
        case SYS_DISPLAY_PROFILE_DUMP:
        case SYS_DISPLAY_VSYNC_SET:
        case SYS_DISPLAY_VSYNC_GET:
        case SYS_SECURITY_LOG_DUMP:
        case SYS_PROCESS_DUMP:
        case SYS_REMOTE_LOGIN_STATS:
        case SYS_NET_SET_LOCAL_IP:
        case SYS_NET_SET_GATEWAY_IP:
        case SYS_REMOTE_LOGIN_STATE:
        case SYS_SCANNER_LOG_READ:
        case SYS_SCANNER_LOG_FLUSH:
        case SYS_SCANNER_LOG_READ_FAT:
        case SYS_SCANNER_LOG_FLUSH_ALL:
        case SYS_SCANNER_LOG_SET_PASSWORD:
        case SYS_HEADLESS_SCANNER_IDLE:
        case SYS_HEADLESS_PROBE_PAUSE_ACTIVE:
        case SYS_AUTH_IS_READY:
        case SYS_AUTH_GET_USERNAME:
        case SYS_AUTH_VERIFY_PASSWORD:
        case SYS_WIFI_INIT:
        case SYS_WIFI_LOAD_FW:
        case SYS_WIFI_UP:
        case SYS_WIFI_DOWN:
        case SYS_WIFI_SCAN:
        case SYS_WIFI_SCAN_SSID:
        case SYS_WIFI_JOIN:
        case SYS_WIFI_RAW_SET_ENABLED:
        case SYS_WIFI_RAW_RECV:
        case SYS_WIFI_RAW_STATUS:
        case SYS_WIFI_MONITOR_SET:
        case SYS_WIFI_MONITOR_STATUS:
        case SYS_WIFI_MONITOR_RECOVER:
        case SYS_WIFI_UP_MONITOR:
        case SYS_WIFI_RANDOMIZE_MAC:
        case SYS_LED_TEST:
        case SYS_LED_SET:
        case SYS_LED_STATUS:
        case SYS_WIFI_DUMP_STATUS:
        case SYS_WIFI_GET_VERSION:
        case SYS_USB_DUMP_INFO:
            req_scope_any = TRUST_SCOPE_SHELL;
            req_role_any = TRUST_ROLE_ADMIN;
            break;

        default:
            return 0;
    }

    if ((proc->signer_scope_mask & req_scope_any) == 0u){
        return 0;
    }
    if ((proc->signer_role_mask & req_role_any) == 0u){
        return 0;
    }
    return 1;
}

void* syscall_handle(void* frame_sp, unsigned long esr){
    unsigned long ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
    unsigned long* frame = (unsigned long*)frame_sp;

    if (ec != ESR_EC_SVC64 || !frame){
        return frame_sp;
    }

    unsigned long nr = frame[TF_X8];
    process_t* caller = get_current_process();
    if (!syscall_capability_allowed(caller, nr)){
        frame[TF_X0] = (unsigned long)-1;
        return frame_sp;
    }

    switch (nr){
        case SYS_PUTC:
            terminal_putc_for_pid(process_current_pid(), (char)frame[TF_X0]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_PUTS:
        {
            char tmp[1025];
            tmp[0] = 0;
            if (copy_cstr_from_user_bound(tmp, sizeof(tmp), (const char*)frame[TF_X0]) == 0){
                syscall_write_puts(process_current_pid(), tmp);
            }
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_TERM_SET_INPUT_LINE:
        {
            char tmp[257];
            tmp[0] = 0;
            if (copy_cstr_from_user_bound(tmp, sizeof(tmp), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            unsigned long len = clamp_puts_len(tmp);
            frame[TF_X0] = (unsigned long)terminal_set_input_overlay_for_pid(process_current_pid(),
                                                                              tmp,
                                                                              len,
                                                                              (unsigned int)len);
            return frame_sp;
        }

        case SYS_TERM_CLEAR_INPUT_LINE:
            frame[TF_X0] = (unsigned long)terminal_clear_input_overlay_for_pid(process_current_pid());
            return frame_sp;

        case SYS_SLEEP: {
            unsigned int ms = (unsigned int)frame[TF_X0];
            syscall_poll_background_io();
            frame[TF_X0] = 0;
            return process_sleep_on_frame(ms, frame_sp);
        }

        case SYS_EXIT:
            process_exit_current();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_FB_CLEAR:
            frame[TF_X0] = (unsigned long)display_clear_for_pid(process_current_pid(),
                                                                 (unsigned int)frame[TF_X0]);
            return frame_sp;

        case SYS_FB_GET_WIDTH:
            frame[TF_X0] = fb_get_width();
            return frame_sp;

        case SYS_FB_GET_HEIGHT:
            frame[TF_X0] = fb_get_height();
            return frame_sp;

        case SYS_FB_RECT:
            frame[TF_X0] = (unsigned long)display_rect_for_pid(process_current_pid(),
                                                               (unsigned int)frame[TF_X0],
                                                               (unsigned int)frame[TF_X1],
                                                               (unsigned int)frame[TF_X2],
                                                               (unsigned int)frame[TF_X3],
                                                               (unsigned int)frame[TF_X4]);
            return frame_sp;

        case SYS_FB_PRESENT:
            frame[TF_X0] = (unsigned long)display_present_for_pid(process_current_pid());
            return frame_sp;

        case SYS_FB_BLIT_RGBA: {
            unsigned int x = (unsigned int)frame[TF_X0];
            unsigned int y = (unsigned int)frame[TF_X1];
            unsigned int w = (unsigned int)frame[TF_X2];
            unsigned int h = (unsigned int)frame[TF_X3];
            const unsigned char* user_src = (const unsigned char*)frame[TF_X4];
            int pid = process_current_pid();

            if (!user_src || w == 0u || h == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            unsigned long row_bytes = (unsigned long)w * 4ul;
            if (row_bytes == 0ul){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (row_bytes > 0xFFFFFFFFul){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if ((~0ul / row_bytes) < (unsigned long)h){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            unsigned long total_bytes = row_bytes * (unsigned long)h;
            if (!process_user_range_readable(user_src, total_bytes)){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            frame[TF_X0] = (unsigned long)display_blit_rgba32_for_pid(pid,
                                                                      x,
                                                                      y,
                                                                      w,
                                                                      h,
                                                                      user_src,
                                                                      (unsigned int)row_bytes);
            return frame_sp;
        }

        case SYS_FB_BLIT_NATIVE: {
            unsigned int x = (unsigned int)frame[TF_X0];
            unsigned int y = (unsigned int)frame[TF_X1];
            unsigned int w = (unsigned int)frame[TF_X2];
            unsigned int h = (unsigned int)frame[TF_X3];
            const unsigned int* user_src = (const unsigned int*)frame[TF_X4];
            int pid = process_current_pid();

            if (!user_src || w == 0u || h == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            unsigned long row_bytes = (unsigned long)w * sizeof(unsigned int);
            if (row_bytes == 0ul || row_bytes > 0xFFFFFFFFul){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if ((~0ul / row_bytes) < (unsigned long)h){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            unsigned long total_bytes = row_bytes * (unsigned long)h;
            if (!process_user_range_readable(user_src, total_bytes)){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            frame[TF_X0] = (unsigned long)display_blit_native32_for_pid(pid,
                                                                        x,
                                                                        y,
                                                                        w,
                                                                        h,
                                                                        user_src,
                                                                        (unsigned int)row_bytes);
            return frame_sp;
        }

        case SYS_FB_ATTACH_BUFFER: {
            unsigned int* user_pixels = (unsigned int*)frame[TF_X0];
            unsigned int w = (unsigned int)frame[TF_X1];
            unsigned int h = (unsigned int)frame[TF_X2];
            unsigned int pitch = (unsigned int)frame[TF_X3];
            int pid = process_current_pid();

            if (!user_pixels || w == 0u || h == 0u ||
                w != fb_get_width() || h != fb_get_height()){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            unsigned long row_bytes = (unsigned long)w * sizeof(unsigned int);
            if (row_bytes == 0ul || row_bytes > 0xFFFFFFFFul ||
                pitch < row_bytes){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if ((~0ul / (unsigned long)pitch) < (unsigned long)h){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            unsigned long total_bytes = (unsigned long)pitch * (unsigned long)h;
            if (!process_user_range_writable(user_pixels, total_bytes)){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            frame[TF_X0] = (unsigned long)display_attach_external_framebuffer_for_pid(pid,
                                                                                       user_pixels,
                                                                                       w,
                                                                                       h,
                                                                                       pitch,
                                                                                       total_bytes);
            return frame_sp;
        }

        case SYS_FB_DIRECT_ACQUIRE: {
            qos_fb_direct_info_t info;
            qos_fb_direct_info_t* user_info = (qos_fb_direct_info_t*)frame[TF_X0];
            int pid = process_current_pid();
            unsigned int page_count = fb_get_page_count();
            unsigned int visible_page = fb_get_display_page();
            unsigned int page = 1u;
            unsigned int page_b = 2u;
            unsigned int map_first_page = 1u;
            unsigned int map_last_page = 2u;
            unsigned long base;
            unsigned long map_base;
            unsigned int pitch = fb_get_pitch();
            unsigned int width = fb_get_width();
            unsigned int height = fb_get_height();
            unsigned long size = (unsigned long)pitch * (unsigned long)height;
            unsigned long map_size;

            if (page_count >= 3u){
                page = 1u;
                page_b = 2u;
                map_first_page = 1u;
                map_last_page = 2u;
            } else if (page_count == 2u){
                /*
                 * Pi 3 commonly only gives us two pages. While active, the
                 * graphics app can use both pages for fullscreen flipping.
                 * When inactive, the SDL shim drops draw work so page 0 can
                 * safely return to tty0.
                 */
                page = (visible_page == 0u) ? 1u : 0u;
                page_b = (page == 0u) ? 1u : 0u;
                map_first_page = 0u;
                map_last_page = 1u;
            }

            base = fb_get_page_base(page);
            map_base = fb_get_page_base(map_first_page);
            map_size = size * ((unsigned long)(map_last_page - map_first_page) + 1UL);

            /*
             * Direct userspace gets one or two mapped HDMI pages. The SDL shim
             * must stop using those pointers while the graphics session is not
             * active, because tty0 owns page 0 when the shell is visible.
             */
            if (!user_info || page_count < 2u || !base || !map_base ||
                pitch == 0u || width == 0u || height == 0u || size == 0UL ||
                (map_base & 0xFFFUL) != 0UL || map_size < size){
                frame[TF_X0] = (unsigned long)-2;
                return frame_sp;
            }
            if (!process_user_range_writable(user_info, sizeof(*user_info))){
                frame[TF_X0] = (unsigned long)-3;
                return frame_sp;
            }
            if (mmu_process_map_framebuffer(pid, map_base, map_size) != 0){
                frame[TF_X0] = (unsigned long)-4;
                return frame_sp;
            }

            unsigned int* fb_words = (unsigned int*)map_base;
            unsigned long words = map_size / sizeof(unsigned int);
            for (unsigned long i = 0; i < words; i++){
                fb_words[i] = 0u;
            }

            if (display_attach_direct_framebuffer_for_pid(pid, page, page_b) != 0){
                frame[TF_X0] = (unsigned long)-5;
                return frame_sp;
            }

            info.pixels = (unsigned int*)base;
            info.width = width;
            info.height = height;
            info.pitch = pitch;
            info.page = page;
            frame[TF_X0] = (process_copy_to_user(user_info,
                                                 &info,
                                                 sizeof(info)) == 0) ? 0ul : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_FB_DIRECT_PRESENT: {
            qos_fb_direct_info_t info;
            qos_fb_direct_info_t* user_info = (qos_fb_direct_info_t*)frame[TF_X0];
            int pid = process_current_pid();
            unsigned int next_page = 0u;
            unsigned long next_base = 0UL;
            unsigned int pitch = fb_get_pitch();
            unsigned int width = fb_get_width();
            unsigned int height = fb_get_height();
            unsigned long size = (unsigned long)pitch * (unsigned long)height;

            if (!user_info || !process_user_range_writable(user_info, sizeof(*user_info)) ||
                pitch == 0u || width == 0u || height == 0u || size == 0UL){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (display_direct_present_for_pid(pid, &next_page) != 0 ||
                next_page >= fb_get_page_count()){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            next_base = fb_get_page_base(next_page);
            if (!next_base ||
                (next_base & 0xFFFUL) != 0UL ||
                mmu_process_map_framebuffer(pid, next_base, size) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            info.pixels = (unsigned int*)next_base;
            info.width = width;
            info.height = height;
            info.pitch = pitch;
            info.page = next_page;
            frame[TF_X0] = (process_copy_to_user(user_info,
                                                 &info,
                                                 sizeof(info)) == 0) ? 0ul : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_FB_DIRECT_GET_DRAW: {
            qos_fb_direct_info_t info;
            qos_fb_direct_info_t* user_info = (qos_fb_direct_info_t*)frame[TF_X0];
            int pid = process_current_pid();
            unsigned int draw_page = 0u;
            unsigned long draw_base = 0UL;
            unsigned int pitch = fb_get_pitch();
            unsigned int width = fb_get_width();
            unsigned int height = fb_get_height();
            unsigned long size = (unsigned long)pitch * (unsigned long)height;

            if (!user_info || !process_user_range_writable(user_info, sizeof(*user_info)) ||
                pitch == 0u || width == 0u || height == 0u || size == 0UL){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = display_direct_get_draw_for_pid(pid, &draw_page);
            if (rc != 0){
                frame[TF_X0] = (unsigned long)rc;
                return frame_sp;
            }
            draw_base = fb_get_page_base(draw_page);
            if (!draw_base ||
                (draw_base & 0xFFFUL) != 0UL ||
                mmu_process_map_framebuffer(pid, draw_base, size) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            info.pixels = (unsigned int*)draw_base;
            info.width = width;
            info.height = height;
            info.pitch = pitch;
            info.page = draw_page;
            frame[TF_X0] = (process_copy_to_user(user_info,
                                                 &info,
                                                 sizeof(info)) == 0) ? 0ul : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_GPU2D_STATUS:
            frame[TF_X0] = (unsigned long)gpu2d_status();
            return frame_sp;

        case SYS_GPU2D_BLIT_RGBA: {
            qos_gpu2d_blit_t blit;
            const qos_gpu2d_blit_t* user_blit = (const qos_gpu2d_blit_t*)frame[TF_X0];
            if (!user_blit ||
                process_copy_from_user(&blit, user_blit, sizeof(blit)) != 0 ||
                validate_gpu2d_blit_source(&blit) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)gpu2d_blit_rgba_for_pid(process_current_pid(), &blit);
            return frame_sp;
        }

        case SYS_GPU2D_BLIT_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_blit_count();
            return frame_sp;

        case SYS_GPU2D_FALLBACK_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_fallback_count();
            return frame_sp;

        case SYS_GPU2D_UNSUPPORTED_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_unsupported_count();
            return frame_sp;

        case SYS_GPU2D_CLEAR:
            frame[TF_X0] = (unsigned long)gpu2d_clear_for_pid(process_current_pid(),
                                                              (unsigned int)frame[TF_X0]);
            return frame_sp;

        case SYS_GPU2D_CLEAR_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_clear_count();
            return frame_sp;

        case SYS_GPU2D_FILL_RECT:
            frame[TF_X0] = (unsigned long)gpu2d_fill_rect_for_pid(process_current_pid(),
                                                                  (unsigned int)frame[TF_X0],
                                                                  (unsigned int)frame[TF_X1],
                                                                  (unsigned int)frame[TF_X2],
                                                                  (unsigned int)frame[TF_X3],
                                                                  (unsigned int)frame[TF_X4]);
            return frame_sp;

        case SYS_GPU2D_FILL_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_fill_count();
            return frame_sp;

        case SYS_GPU2D_QUAD_BATCH: {
            const qos_gpu2d_quad_t* user_quads = (const qos_gpu2d_quad_t*)frame[TF_X0];
            unsigned int count = (unsigned int)frame[TF_X1];
            if (!user_quads || count == 0u || count > QOS_GPU2D_QUAD_BATCH_MAX){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            unsigned long bytes = (unsigned long)count * sizeof(qos_gpu2d_quad_t);
            qos_gpu2d_quad_t* quads = (qos_gpu2d_quad_t*)kmalloc(bytes);
            if (!quads){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (process_copy_from_user(quads, user_quads, bytes) != 0){
                kfree_secure(quads, bytes);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = gpu2d_submit_quads_for_pid(process_current_pid(), quads, count);
            kfree_secure(quads, bytes);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_GPU2D_QUAD_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_quad_count();
            return frame_sp;

        case SYS_GPU2D_QUAD_BATCH_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_quad_batch_count();
            return frame_sp;

        case SYS_GPU2D_QPU_SET_ENABLED:
            frame[TF_X0] = (unsigned long)gpu2d_qpu_set_enabled(frame[TF_X0] ? 1 : 0);
            return frame_sp;

        case SYS_GPU2D_QPU_STATUS:
            frame[TF_X0] = (unsigned long)gpu2d_qpu_is_enabled();
            return frame_sp;

        case SYS_GPU2D_QPU_QUAD_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_qpu_quad_count();
            return frame_sp;

        case SYS_GPU2D_QPU_FAIL_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_qpu_fail_count();
            return frame_sp;

        case SYS_GPU2D_V3D_SET_ENABLED:
            frame[TF_X0] = (unsigned long)gpu2d_v3d_set_enabled(frame[TF_X0] ? 1 : 0);
            return frame_sp;

        case SYS_GPU2D_V3D_STATUS:
            frame[TF_X0] = (unsigned long)gpu2d_v3d_is_enabled();
            return frame_sp;

        case SYS_GPU2D_V3D_QUAD_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_v3d_quad_count();
            return frame_sp;

        case SYS_GPU2D_V3D_BATCH_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_v3d_batch_count();
            return frame_sp;

        case SYS_GPU2D_V3D_FAIL_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_v3d_fail_count();
            return frame_sp;

        case SYS_GPU2D_TEXTURE_UPLOAD: {
            qos_gpu2d_texture_upload_t req;
            qos_gpu2d_texture_upload_t* user_req = (qos_gpu2d_texture_upload_t*)frame[TF_X0];
            if (!user_req ||
                process_copy_from_user(&req, user_req, sizeof(req)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = gpu2d_texture_upload_for_pid(process_current_pid(), &req);
            if (rc == 0){
                /*
                 * Only return the generated handle. Keeping this writeback
                 * narrow prevents future ABI changes from accidentally
                 * refreshing userspace pointer fields with kernel-side state.
                 */
                if (process_copy_to_user((void*)&user_req->texture_id,
                                         &req.texture_id,
                                         sizeof(req.texture_id)) != 0){
                    (void)gpu2d_texture_free_for_pid(process_current_pid(), req.texture_id);
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_GPU2D_TEXTURE_FREE:
            frame[TF_X0] = (unsigned long)gpu2d_texture_free_for_pid(process_current_pid(),
                                                                     (unsigned int)frame[TF_X0]);
            return frame_sp;

        case SYS_GPU2D_TEXTURE_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_texture_count();
            return frame_sp;

        case SYS_GPU2D_TEXTURE_UPLOAD_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_texture_upload_count();
            return frame_sp;

        case SYS_GPU2D_TEXTURE_FREE_COUNT:
            frame[TF_X0] = (unsigned long)gpu2d_texture_free_count();
            return frame_sp;

        case SYS_GPU2D_TEXTURE_BYTES:
            frame[TF_X0] = (unsigned long)gpu2d_texture_bytes();
            return frame_sp;

        case SYS_V3D_PROBE: {
            qos_v3d_status_t st;
            qos_v3d_status_t* user_st = (qos_v3d_status_t*)frame[TF_X0];
            int rc = v3d_probe(&st);
            if (!user_st || process_copy_to_user(user_st, &st, sizeof(st)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_V3D_STATUS: {
            qos_v3d_status_t st;
            qos_v3d_status_t* user_st = (qos_v3d_status_t*)frame[TF_X0];
            int rc = v3d_get_status(&st);
            if (!user_st || process_copy_to_user(user_st, &st, sizeof(st)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_V3D_NOOP: {
            qos_v3d_status_t st;
            unsigned int thread = (unsigned int)frame[TF_X0];
            qos_v3d_status_t* user_st = (qos_v3d_status_t*)frame[TF_X1];
            int rc = v3d_submit_noop(thread, &st);
            if (!user_st || process_copy_to_user(user_st, &st, sizeof(st)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_V3D_CLEAR: {
            qos_v3d_status_t st;
            unsigned int rgba = (unsigned int)frame[TF_X0];
            qos_v3d_status_t* user_st = (qos_v3d_status_t*)frame[TF_X1];
            int rc = v3d_clear_visible(rgba, &st);
            if (!user_st || process_copy_to_user(user_st, &st, sizeof(st)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_V3D_QPU_PROBE: {
            qos_v3d_status_t st;
            qos_v3d_status_t* user_st = (qos_v3d_status_t*)frame[TF_X0];
            int rc = v3d_qpu_memory_probe(&st);
            if (!user_st || process_copy_to_user(user_st, &st, sizeof(st)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_V3D_QPU_WRITE_PROBE: {
            qos_v3d_status_t st;
            qos_v3d_status_t* user_st = (qos_v3d_status_t*)frame[TF_X0];
            int rc = v3d_qpu_memory_write_probe(&st);
            if (!user_st || process_copy_to_user(user_st, &st, sizeof(st)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_V3D_QPU_EXEC_PROBE: {
            qos_v3d_status_t st;
            qos_v3d_status_t* user_st = (qos_v3d_status_t*)frame[TF_X0];
            int rc = v3d_qpu_execute_probe(&st);
            if (!user_st || process_copy_to_user(user_st, &st, sizeof(st)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TRY_GETC: {
            char c = 0;
            syscall_poll_background_io();
            if (terminal_try_getc_for_pid(process_current_pid(), &c)){
                frame[TF_X0] = (unsigned long)(unsigned char)c;
            } else{
                frame[TF_X0] = (unsigned long)-1;
            }
            return frame_sp;
        }

        case SYS_TRY_GETC_EX: {
            char c = 0;
            unsigned int src = 0u;
            syscall_poll_background_io();
            if (terminal_try_getc_for_pid_ex(process_current_pid(), &c, &src)){
                frame[TF_X0] = ((unsigned long)(src & 0xFFu) << 8) |
                               (unsigned long)((unsigned char)c);
            } else{
                frame[TF_X0] = (unsigned long)-1;
            }
            return frame_sp;
        }

        case SYS_INPUT_POLL_EVENT: {
            qos_event_t ev;
            int pid = process_current_pid();
            if (!frame[TF_X0] || !display_is_active_graphics_pid(pid)){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            if (!usb_host_poll_event(&ev)){
                frame[TF_X0] = 0;
                return frame_sp;
            }

            frame[TF_X0] = (process_copy_to_user((void*)frame[TF_X0],
                                                 &ev,
                                                 sizeof(ev)) == 0) ? 1ul : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_FILE_READ_BMP: {
            char sandbox83[11];
            char path[USER_BMP_PATH_MAX];
            unsigned char* user_out = (unsigned char*)frame[TF_X1];
            unsigned int out_cap = (unsigned int)frame[TF_X2];
            unsigned char* kbuf = 0;
            int n = -1;
            unsigned long t_total = profile_time_us();
            unsigned long t0;
            unsigned long cstr_us = 0;
            unsigned long alloc_us = 0;
            unsigned long fat_init_us = 0;
            unsigned long fat_read_us = 0;
            unsigned long copy_us = 0;

            if (!frame[TF_X0] || !user_out || out_cap == 0u || out_cap > USER_BMP_FILE_MAX){
                file_profile_note(0, 0, 0, 0, 0, 0, 0, profile_time_us() - t_total);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (process_current_file_sandbox(sandbox83) != 0){
                file_profile_note(0, 0, 0, 0, 0, 0, 0, profile_time_us() - t_total);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(path); i++){
                path[i] = 0;
            }
            t0 = profile_time_us();
            if (process_copy_cstr_from_user(path, sizeof(path), (const char*)frame[TF_X0]) != 0){
                cstr_us = profile_time_us() - t0;
                file_profile_note(0, 0, cstr_us, 0, 0, 0, 0, profile_time_us() - t_total);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            cstr_us = profile_time_us() - t0;

            t0 = profile_time_us();
            kbuf = (unsigned char*)kmalloc((unsigned long)out_cap);
            alloc_us = profile_time_us() - t0;
            if (!kbuf){
                file_profile_note(0, 0, cstr_us, alloc_us, 0, 0, 0, profile_time_us() - t_total);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kernel_preempt_enter();
            t0 = profile_time_us();
            int fat_init_rc = fat32_init();
            fat_init_us = profile_time_us() - t0;
            if (fat_init_rc == 0){
                t0 = profile_time_us();
                n = fat32_read_file_in_dir_path(sandbox83, path, kbuf, (int)out_cap);
                fat_read_us = profile_time_us() - t0;
            }
            kernel_preempt_exit();
            if (n < 0 || n > (int)out_cap){
                kfree(kbuf);
                file_profile_note(0, 0, cstr_us, alloc_us, fat_init_us, fat_read_us, 0, profile_time_us() - t_total);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            t0 = profile_time_us();
            if (process_copy_to_user(user_out, kbuf, (unsigned long)n) != 0){
                copy_us = profile_time_us() - t0;
                kfree(kbuf);
                file_profile_note(0, 0, cstr_us, alloc_us, fat_init_us, fat_read_us, copy_us, profile_time_us() - t_total);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            copy_us = profile_time_us() - t0;
            kfree(kbuf);
            file_profile_note(1, (unsigned long)n, cstr_us, alloc_us, fat_init_us, fat_read_us, copy_us, profile_time_us() - t_total);
            frame[TF_X0] = (unsigned long)n;
            return frame_sp;
        }

        case SYS_FILE_APPEND_DATA: {
            char sandbox83[11];
            char path[USER_BMP_PATH_MAX];
            const unsigned char* user_in = (const unsigned char*)frame[TF_X1];
            unsigned int in_len = (unsigned int)frame[TF_X2];
            unsigned char* kbuf = 0;
            int rc = -1;

            if (!frame[TF_X0] || !user_in || in_len == 0u || in_len > SANDBOX_FILE_WRITE_MAX){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (process_current_file_sandbox(sandbox83) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(path); i++){
                path[i] = 0;
            }
            if (process_copy_cstr_from_user(path, sizeof(path), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kbuf = (unsigned char*)kmalloc((unsigned long)in_len);
            if (!kbuf || process_copy_from_user(kbuf, user_in, in_len) != 0){
                if (kbuf){
                    kfree_secure(kbuf, in_len);
                }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            rc = sandbox_file_append(sandbox83, path, kbuf, in_len);
            kfree_secure(kbuf, in_len);
            frame[TF_X0] = (rc >= 0) ? (unsigned long)rc : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_FILE_READ_DATA: {
            char sandbox83[11];
            char path[USER_BMP_PATH_MAX];
            unsigned char* user_out = (unsigned char*)frame[TF_X1];
            unsigned int out_cap = (unsigned int)frame[TF_X2];
            unsigned char* kbuf = 0;
            int n = -1;

            if (!frame[TF_X0] || !user_out || out_cap == 0u || out_cap > USER_FILE_RW_MAX){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (process_current_file_sandbox(sandbox83) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(path); i++){
                path[i] = 0;
            }
            if (process_copy_cstr_from_user(path, sizeof(path), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kbuf = (unsigned char*)kmalloc((unsigned long)out_cap);
            if (!kbuf){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            n = sandbox_file_read(sandbox83, path, kbuf, out_cap);
            if (n > 0 && process_copy_to_user(user_out, kbuf, (unsigned long)n) != 0){
                kfree_secure(kbuf, out_cap);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kfree_secure(kbuf, out_cap);
            frame[TF_X0] = (n >= 0) ? (unsigned long)n : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_FILE_SIZE: {
            char sandbox83[11];
            char path[USER_BMP_PATH_MAX];
            int n = -1;
            if (!frame[TF_X0]){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (process_current_file_sandbox(sandbox83) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(path); i++){
                path[i] = 0;
            }
            if (process_copy_cstr_from_user(path, sizeof(path), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            n = sandbox_file_size(sandbox83, path);
            frame[TF_X0] = (n >= 0) ? (unsigned long)n : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_FILE_CLEAR: {
            char sandbox83[11];
            char path[USER_BMP_PATH_MAX];
            int rc = -1;
            if (!frame[TF_X0]){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (process_current_file_sandbox(sandbox83) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(path); i++){
                path[i] = 0;
            }
            if (process_copy_cstr_from_user(path, sizeof(path), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            rc = sandbox_file_clear(sandbox83, path);
            frame[TF_X0] = (rc == 0) ? 0ul : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_FILE_PROFILE_RESET:
            file_profile_reset();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_FILE_PROFILE_DUMP:
            file_profile_dump();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_RUN_PROGRAM: {
            kernel_preempt_enter();
            loaded_program_t prog = load_program_from_sd();
            if (!prog.entry){
                kernel_preempt_exit();
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            int pid = process_create_loaded(prog);
            if (pid < 0){
                if (prog.heap_allocated){
                    kfree_secure(prog.memory, prog.size);
                } else{
                    volatile unsigned char* m = (volatile unsigned char*)prog.memory;
                    for (unsigned long i = 0; i < prog.size; i++){
                        m[i] = 0;
                    }
                    loader_free_program_memory(prog.memory, prog.size);
                }
                kernel_preempt_exit();
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            (void)terminal_attach_pid(pid, terminal_get_for_pid(process_current_pid()));
            kernel_preempt_exit();
            frame[TF_X0] = (unsigned long)pid;
            return frame_sp;
        }

        case SYS_RUN_PROGRAM_NAMED: {
            const char* fat_name_83 = (const char*)frame[TF_X0];
            char fat_name_local[12];
            if (!fat_name_83 || process_copy_from_user(fat_name_local, fat_name_83, 11u) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            fat_name_local[11] = 0;

            kernel_preempt_enter();
            loaded_program_t prog = load_program_from_sd_named(fat_name_local);
            if (!prog.entry){
                kernel_preempt_exit();
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            int pid = process_create_loaded(prog);
            if (pid < 0){
                if (prog.heap_allocated){
                    kfree_secure(prog.memory, prog.size);
                } else{
                    volatile unsigned char* m = (volatile unsigned char*)prog.memory;
                    for (unsigned long i = 0; i < prog.size; i++){
                        m[i] = 0;
                    }
                    loader_free_program_memory(prog.memory, prog.size);
                }
                kernel_preempt_exit();
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            (void)terminal_attach_pid(pid, terminal_get_for_pid(process_current_pid()));
            kernel_preempt_exit();
            frame[TF_X0] = (unsigned long)pid;
            return frame_sp;
        }

        case SYS_DISPLAY_CREATE_GRAPHICS: {
            int target = (int)frame[TF_X0];
            process_t* target_proc = get_process(target);
            if (!target_proc ||
                target_proc->state == PROC_DEAD ||
                target_proc->state == PROC_REAPING){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)display_create_graphics_session(target);
            return frame_sp;
        }

        case SYS_DISPLAY_SWITCH_GRAPHICS: {
            int target = (int)frame[TF_X0];
            process_t* target_proc = get_process(target);
            if (!target_proc ||
                target_proc->state == PROC_DEAD ||
                target_proc->state == PROC_REAPING){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)display_set_active_for_pid(target);
            return frame_sp;
        }

        case SYS_DISPLAY_SWITCH_SESSION: {
            int pid = process_current_pid();
            int session_id = (int)frame[TF_X0];
            int rc = terminal_switch_display_session(session_id);
            if (rc == 0 && session_id == DISPLAY_TEXT_SESSION_ID && pid >= 0){
                (void)terminal_attach_pid(pid, 0);
                (void)terminal_set_foreground_pid(0, pid);
                (void)console_set_owner(pid, pid);
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_PROCESS_KILL: {
            int target = (int)frame[TF_X0];
            process_t* target_proc = get_process(target);
            if (target <= 0 ||
                !target_proc ||
                target_proc->state == PROC_DEAD ||
                target_proc->state == PROC_REAPING){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            process_exit(target);
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_PROCESS_STATE: {
            int target = (int)frame[TF_X0];
            process_t* target_proc = get_process(target);
            if (target < 0 || !target_proc){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)target_proc->state;
            return frame_sp;
        }

        case SYS_PROCESS_LOG_READ: {
            int target = (int)frame[TF_X0];
            char* user_out = (char*)frame[TF_X1];
            unsigned int out_cap = (unsigned int)frame[TF_X2];
            if (!user_out || out_cap == 0u || out_cap > 4096u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            char* tmp = (char*)kmalloc(out_cap);
            if (!tmp){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int n = terminal_log_read(target, tmp, out_cap);
            if (n >= 0){
                unsigned int copy_len = (unsigned int)n + 1u;
                if (copy_len > out_cap){
                    copy_len = out_cap;
                }
                if (process_copy_to_user(user_out, tmp, copy_len) != 0){
                    n = -1;
                }
            }
            kfree(tmp);
            frame[TF_X0] = (unsigned long)n;
            return frame_sp;
        }

        case SYS_NET_DUMP_STATS:
            (void)net_poll();
            remote_login_poll();
            net_dump_stats();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_NET_SEND_TEST_FRAME:
            frame[TF_X0] = (unsigned long)net_send_test_frame();
            return frame_sp;

        case SYS_NET_POLL:
            frame[TF_X0] = (unsigned long)net_poll();
            return frame_sp;

        case SYS_NET_RECV_RAW:
        {
            unsigned char* user_out = (unsigned char*)frame[TF_X0];
            unsigned int cap = clamp_u32((unsigned int)frame[TF_X1], NET_MAX_FRAME_SIZE);
            unsigned char kbuf[NET_MAX_FRAME_SIZE];
            int n = net_recv_raw(kbuf, cap);
            if (n > 0){
                if (process_copy_to_user(user_out, kbuf, (unsigned long)n) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
            }
            frame[TF_X0] = (unsigned long)n;
            return frame_sp;
        }

        case SYS_NET_SEND_RAW:
        {
            const unsigned char* user_frame = (const unsigned char*)frame[TF_X0];
            unsigned int len = (unsigned int)frame[TF_X1];
            if (len == 0u || len > NET_MAX_FRAME_SIZE){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            unsigned char kbuf[NET_MAX_FRAME_SIZE];
            if (process_copy_from_user(kbuf, user_frame, len) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)net_send_raw(kbuf, len);
            return frame_sp;
        }

        case SYS_USB_DUMP_INFO:
            syscall_dump_usb_info();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_NET_PING_GATEWAY:
            kernel_preempt_enter();
            frame[TF_X0] = (unsigned long)net_ping_gateway((unsigned int)frame[TF_X0]);
            kernel_preempt_exit();
            return frame_sp;

        case SYS_NET_UDP_SEND_PROBE:
            frame[TF_X0] = (unsigned long)udp_send_probe_gateway();
            return frame_sp;

        case SYS_NET_UDP_RECV:
        {
            udp_meta_t kmeta;
            unsigned char kbuf[UDP_MAX_PAYLOAD];
            udp_meta_t* user_meta = (udp_meta_t*)frame[TF_X0];
            unsigned char* user_out = (unsigned char*)frame[TF_X1];
            unsigned int cap = clamp_u32((unsigned int)frame[TF_X2], UDP_MAX_PAYLOAD);
            int n = udp_recv_next(kbuf, cap, &kmeta);
            if (n > 0){
                if (process_copy_to_user(user_out, kbuf, (unsigned long)n) != 0 ||
                    process_copy_to_user(user_meta, &kmeta, sizeof(kmeta)) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
            }
            frame[TF_X0] = (unsigned long)n;
            return frame_sp;
        }

        case SYS_NET_UDP_SEND:
        {
            unsigned char dst_ip[4];
            unsigned int len = (unsigned int)frame[TF_X4];
            if (len > UDP_MAX_PAYLOAD){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            unsigned char kbuf[UDP_MAX_PAYLOAD];
            if (process_copy_from_user(dst_ip, (const void*)frame[TF_X0], 4u) != 0 ||
                process_copy_from_user(kbuf, (const void*)frame[TF_X3], len) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)udp_send(dst_ip,
                                                   (unsigned short)frame[TF_X1],
                                                   (unsigned short)frame[TF_X2],
                                                   kbuf,
                                                   len);
            return frame_sp;
        }

        case SYS_NET_TCP_HTTP_GET:
        {
            unsigned char dst_ip[4];
            char host[USER_CSTR_MAX];
            char path[USER_CSTR_MAX];
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X4], USER_IO_MAX);
            unsigned char* user_out = (unsigned char*)frame[TF_X3];
            unsigned char* kout = 0;

            if (out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (process_copy_from_user(dst_ip, (const void*)frame[TF_X0], 4u) != 0 ||
                copy_cstr_from_user_bound(host, sizeof(host), (const char*)frame[TF_X1]) != 0 ||
                copy_cstr_from_user_bound(path, sizeof(path), (const char*)frame[TF_X2]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kout = (unsigned char*)kmalloc(out_cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kernel_preempt_enter();
            int rc = tcp_http_get(dst_ip, host, path, kout, out_cap);
            kernel_preempt_exit();
            if (rc > 0 && process_copy_to_user(user_out, kout, (unsigned long)rc) != 0){
                rc = -1;
            }
            kfree(kout);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_SOCKET_CREATE: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ksocket_create(pid,
                                                         (int)frame[TF_X0],
                                                         (int)frame[TF_X1],
                                                         (int)frame[TF_X2]);
            return frame_sp;
        }

        case SYS_SOCKET_CONNECT: {
            int pid = process_current_pid();
            qos_sockaddr_in_t addr;
            if ((unsigned int)frame[TF_X2] < (unsigned int)sizeof(addr) ||
                process_copy_from_user(&addr, (const void*)frame[TF_X1], sizeof(addr)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)ksocket_connect(pid,
                                                          (int)frame[TF_X0],
                                                          &addr,
                                                          (unsigned int)sizeof(addr));
            return frame_sp;
        }

        case SYS_SOCKET_SEND: {
            int pid = process_current_pid();
            unsigned int len = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned char* kbuf = 0;
            if (len == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kbuf = (unsigned char*)kmalloc(len);
            if (!kbuf || process_copy_from_user(kbuf, (const void*)frame[TF_X1], len) != 0){
                if (kbuf){ kfree(kbuf); }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = ksocket_send(pid,
                                  (int)frame[TF_X0],
                                  kbuf,
                                  len,
                                  (unsigned int)frame[TF_X3]);
            kfree(kbuf);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_SOCKET_RECV: {
            int pid = process_current_pid();
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned char* kout = 0;
            if (out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kout = (unsigned char*)kmalloc(out_cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kernel_preempt_enter();
            int rc = ksocket_recv(pid,
                                  (int)frame[TF_X0],
                                  kout,
                                  out_cap,
                                  (unsigned int)frame[TF_X3]);
            kernel_preempt_exit();
            if (rc > 0 && process_copy_to_user((void*)frame[TF_X1], kout, (unsigned long)rc) != 0){
                rc = -1;
            }
            kfree(kout);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_SOCKET_CLOSE: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ksocket_close(pid, (int)frame[TF_X0]);
            return frame_sp;
        }

        case SYS_SOCKET_SETOPT: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ksocket_setopt(pid,
                                                         (int)frame[TF_X0],
                                                         (int)frame[TF_X1],
                                                         (unsigned int)frame[TF_X2]);
            return frame_sp;
        }

        case SYS_TTY_SET_OWNER: {
            int pid = process_current_pid();
            int target = (int)frame[TF_X0];
            int rc = console_set_owner(pid, target);
            if (rc == 0){
                (void)terminal_set_foreground_pid(terminal_get_for_pid(target), target);
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TTY_RELEASE: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)console_release_owner(pid);
            return frame_sp;
        }

        case SYS_TTY_GET_OWNER:
            frame[TF_X0] = (unsigned long)console_get_owner();
            return frame_sp;

        case SYS_TTY_CLAIM_SELF: {
            int pid = process_current_pid();
            int rc = console_set_owner(pid, pid);
            if (rc == 0){
                (void)terminal_set_foreground_pid(terminal_get_for_pid(pid), pid);
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TERM_GET_ACTIVE:
            frame[TF_X0] = (unsigned long)terminal_get_active();
            return frame_sp;

        case SYS_TERM_SWITCH: {
            int pid = process_current_pid();
            int term_id = (int)frame[TF_X0];
            int rc = terminal_set_active(term_id);
            if (rc == 0 && pid >= 0){
                (void)terminal_attach_pid(pid, term_id);
                (void)terminal_set_foreground_pid(term_id, pid);
                (void)console_set_owner(pid, pid);
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TERM_CLEAR:
            terminal_clear_active();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_TERM_GET_OUTPUT:
            frame[TF_X0] = (unsigned long)terminal_get_active_output();
            return frame_sp;

        case SYS_TERM_SET_OUTPUT:
            frame[TF_X0] = (unsigned long)terminal_set_active_output((unsigned int)frame[TF_X0]);
            return frame_sp;

        case SYS_DMA_SET_ENABLED:
            dma_set_enabled(frame[TF_X0] ? 1 : 0);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_DMA_STATUS:
            frame[TF_X0] = ((unsigned long)(dma_failure_count() & 0xFFFFu) << 16) |
                           (unsigned long)(dma_is_enabled() ? 1u : 0u);
            return frame_sp;

        case SYS_DMA_LAST_CS:
            frame[TF_X0] = (unsigned long)dma_last_cs();
            return frame_sp;

        case SYS_DMA_LAST_DEBUG:
            frame[TF_X0] = (unsigned long)dma_last_debug();
            return frame_sp;

        case SYS_DMA_TRANSFER_COUNT:
            frame[TF_X0] = (unsigned long)dma_transfer_count();
            return frame_sp;

        case SYS_DMA_LAST_BYTES:
            frame[TF_X0] = (unsigned long)dma_last_bytes();
            return frame_sp;

        case SYS_DMA_LAST_CLEAN_US:
            frame[TF_X0] = (unsigned long)dma_last_clean_us();
            return frame_sp;

        case SYS_DMA_LAST_WAIT_US:
            frame[TF_X0] = (unsigned long)dma_last_wait_us();
            return frame_sp;

        case SYS_DMA_LAST_TOTAL_US:
            frame[TF_X0] = (unsigned long)dma_last_total_us();
            return frame_sp;

        case SYS_GPU_SET_ENABLED:
            frame[TF_X0] = (unsigned long)display_gpu_set_enabled(frame[TF_X0] ? 1 : 0);
            return frame_sp;

        case SYS_GPU_STATUS:
            frame[TF_X0] = (unsigned long)display_gpu_status();
            return frame_sp;

        case SYS_GPU_FLIP_COUNT:
            frame[TF_X0] = (unsigned long)display_gpu_flip_count();
            return frame_sp;

        case SYS_SYSTEM_STATUS: {
            qos_system_status_t st;
            st.ok_mask = 0u;
            st.temp_millic = 0u;
            st.arm_hz = 0u;
            st.core_hz = 0u;
            st.v3d_hz = 0u;
            st.throttled_flags = 0u;

            if (!frame[TF_X0]){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (mailbox_get_temperature(0u, &st.temp_millic) == 0){
                st.ok_mask |= QOS_SYSTEM_STATUS_TEMP_OK;
            }
            if (mailbox_get_clock_rate(3u, &st.arm_hz) == 0){
                st.ok_mask |= QOS_SYSTEM_STATUS_ARM_CLOCK_OK;
            }
            if (mailbox_get_clock_rate(4u, &st.core_hz) == 0){
                st.ok_mask |= QOS_SYSTEM_STATUS_CORE_CLOCK_OK;
            }
            if (mailbox_get_clock_rate(5u, &st.v3d_hz) == 0){
                st.ok_mask |= QOS_SYSTEM_STATUS_V3D_CLOCK_OK;
            }
            if (mailbox_get_throttled(&st.throttled_flags) == 0){
                st.ok_mask |= QOS_SYSTEM_STATUS_THROTTLE_OK;
            }
            frame[TF_X0] = (process_copy_to_user((void*)frame[TF_X0],
                                                 &st,
                                                 sizeof(st)) == 0) ? 0ul : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_SYSTEM_SET_CLOCK: {
            unsigned int clock_id = (unsigned int)frame[TF_X0];
            unsigned int hz = (unsigned int)frame[TF_X1];
            if (clock_id != 3u && clock_id != 4u && clock_id != 5u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)mailbox_set_clock_rate(clock_id, hz);
            return frame_sp;
        }

        case SYS_DISPLAY_PROFILE_RESET:
            display_profile_reset();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_DISPLAY_PROFILE_DUMP:
            display_profile_dump();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_DISPLAY_VSYNC_SET:
            frame[TF_X0] = (unsigned long)display_vsync_set_enabled(frame[TF_X0] ? 1 : 0);
            return frame_sp;

        case SYS_DISPLAY_VSYNC_GET:
            frame[TF_X0] = (unsigned long)display_vsync_is_enabled();
            return frame_sp;

        case SYS_SECURITY_LOG_DUMP:
            klog_dump_to_terminal_for_pid(process_current_pid());
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_PROCESS_DUMP:
            process_dump();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_GETPID:
            frame[TF_X0] = (unsigned long)process_current_pid();
            return frame_sp;

        case SYS_GET_TICKS:
            frame[TF_X0] = system_ticks;
            return frame_sp;

        case SYS_GET_COUNTER_HZ: {
            unsigned long hz = 0;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
            frame[TF_X0] = hz;
            return frame_sp;
        }

        case SYS_GET_COUNTER_CYCLES: {
            unsigned long cyc = 0;
            asm volatile("mrs %0, cntpct_el0" : "=r"(cyc));
            frame[TF_X0] = cyc;
            return frame_sp;
        }

        case SYS_GET_TIME_US: {
            unsigned long hz = 0;
            unsigned long cyc = 0;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
            asm volatile("mrs %0, cntpct_el0" : "=r"(cyc));
            frame[TF_X0] = counter_cycles_to_us(cyc, hz);
            return frame_sp;
        }

        case SYS_GET_TIME_NS: {
            unsigned long hz = 0;
            unsigned long cyc = 0;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
            asm volatile("mrs %0, cntpct_el0" : "=r"(cyc));
            frame[TF_X0] = counter_cycles_to_ns(cyc, hz);
            return frame_sp;
        }

        case SYS_TLS_OPEN: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_open(pid, (int)frame[TF_X0]);
            return frame_sp;
        }

        case SYS_TLS_CLOSE: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_close(pid, (int)frame[TF_X0]);
            return frame_sp;
        }

        case SYS_TLS_GET_LOCAL_PUBLIC: {
            int pid = process_current_pid();
            unsigned char pub[32];
            int rc = ktls_get_local_public(pid, (int)frame[TF_X0], pub);
            if (rc == 0 && process_copy_to_user((void*)frame[TF_X1], pub, sizeof(pub)) != 0){
                rc = -1;
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TLS_SET_PEER_PUBLIC: {
            int pid = process_current_pid();
            unsigned char pub[32];
            if (process_copy_from_user(pub, (const void*)frame[TF_X1], sizeof(pub)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)ktls_set_peer_public(pid,
                                                                (int)frame[TF_X0],
                                                                pub);
            return frame_sp;
        }

        case SYS_TLS_BUILD_CLIENT_HELLO: {
            int pid = process_current_pid();
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned int out_len = 0;
            unsigned char* kout = 0;
            if (out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kout = (unsigned char*)kmalloc(out_cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = ktls_build_client_hello(pid,
                                             (int)frame[TF_X0],
                                             kout,
                                             out_cap,
                                             &out_len);
            if (rc == 0 && process_copy_to_user((void*)frame[TF_X1], kout, out_len) != 0){
                rc = -1;
            }
            kfree(kout);
            frame[TF_X0] = (rc == 0) ? (unsigned long)out_len : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_TLS_PROCESS_SERVER_HELLO: {
            int pid = process_current_pid();
            unsigned int in_len = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned char* kin = 0;
            if (in_len == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kin = (unsigned char*)kmalloc(in_len);
            if (!kin || process_copy_from_user(kin, (const void*)frame[TF_X1], in_len) != 0){
                if (kin){ kfree(kin); }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = ktls_process_server_hello(pid,
                                               (int)frame[TF_X0],
                                               kin,
                                               in_len);
            kfree(kin);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TLS_PROCESS_CLIENT_HELLO_BUILD_SERVER_HELLO: {
            int pid = process_current_pid();
            unsigned int in_len = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X4], USER_IO_MAX);
            unsigned char* kin = 0;
            unsigned char* kout = 0;
            unsigned int out_len = 0;
            if (in_len == 0u || out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kin = (unsigned char*)kmalloc(in_len);
            kout = (unsigned char*)kmalloc(out_cap);
            if (!kin || !kout || process_copy_from_user(kin, (const void*)frame[TF_X1], in_len) != 0){
                if (kin){ kfree(kin); }
                if (kout){ kfree(kout); }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = ktls_process_client_hello_build_server_hello(pid,
                                                                   (int)frame[TF_X0],
                                                                   kin,
                                                                   in_len,
                                                                   kout,
                                                                   out_cap,
                                                                   &out_len);
            if (rc == 0 && process_copy_to_user((void*)frame[TF_X3], kout, out_len) != 0){
                rc = -1;
            }
            kfree(kin);
            kfree(kout);
            frame[TF_X0] = (rc == 0) ? (unsigned long)out_len : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_TLS_RECORD_ENCRYPT: {
            int pid = process_current_pid();
            qos_tls_record_io_t user_io;
            qos_tls_record_io_t kio;
            unsigned char* kin = 0;
            unsigned char* kout = 0;
            int rc = -1;

            if (process_copy_from_user(&user_io, (const void*)frame[TF_X1], sizeof(user_io)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (!user_io.out || user_io.out_cap == 0u || user_io.out_cap > USER_IO_MAX ||
                user_io.in_len > USER_IO_MAX){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            if (user_io.in_len > 0u){
                if (!user_io.in){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
                kin = (unsigned char*)kmalloc(user_io.in_len);
                if (!kin || process_copy_from_user(kin, user_io.in, user_io.in_len) != 0){
                    if (kin){
                        kfree_secure(kin, user_io.in_len);
                    }
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
            }

            kout = (unsigned char*)kmalloc(user_io.out_cap);
            if (!kout){
                if (kin){
                    kfree_secure(kin, user_io.in_len);
                }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kio.inner_type = user_io.inner_type;
            kio.in = kin;
            kio.in_len = user_io.in_len;
            kio.out = kout;
            kio.out_cap = user_io.out_cap;
            kio.out_len = 0u;

            rc = ktls_record_encrypt(pid, (int)frame[TF_X0], &kio);
            if (rc > 0){
                if (kio.out_len > user_io.out_cap ||
                    process_copy_to_user(user_io.out, kout, kio.out_len) != 0){
                    rc = -1;
                }
            }

            user_io.out_len = kio.out_len;
            if (process_copy_to_user((void*)frame[TF_X1], &user_io, sizeof(user_io)) != 0){
                rc = -1;
            }

            kfree_secure(kout, user_io.out_cap);
            if (kin){
                kfree_secure(kin, user_io.in_len);
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TLS_RECORD_DECRYPT: {
            int pid = process_current_pid();
            qos_tls_record_io_t user_io;
            qos_tls_record_io_t kio;
            unsigned char* kin = 0;
            unsigned char* kout = 0;
            int rc = -1;

            if (process_copy_from_user(&user_io, (const void*)frame[TF_X1], sizeof(user_io)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (!user_io.in || !user_io.out ||
                user_io.in_len == 0u || user_io.out_cap == 0u ||
                user_io.in_len > USER_IO_MAX || user_io.out_cap > USER_IO_MAX){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kin = (unsigned char*)kmalloc(user_io.in_len);
            kout = (unsigned char*)kmalloc(user_io.out_cap);
            if (!kin || !kout ||
                process_copy_from_user(kin, user_io.in, user_io.in_len) != 0){
                if (kin){
                    kfree_secure(kin, user_io.in_len);
                }
                if (kout){
                    kfree_secure(kout, user_io.out_cap);
                }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kio.inner_type = user_io.inner_type;
            kio.in = kin;
            kio.in_len = user_io.in_len;
            kio.out = kout;
            kio.out_cap = user_io.out_cap;
            kio.out_len = 0u;

            rc = ktls_record_decrypt(pid, (int)frame[TF_X0], &kio);
            if (rc > 0){
                if (kio.out_len > user_io.out_cap ||
                    process_copy_to_user(user_io.out, kout, kio.out_len) != 0){
                    rc = -1;
                }
            }

            user_io.inner_type = kio.inner_type;
            user_io.out_len = kio.out_len;
            if (process_copy_to_user((void*)frame[TF_X1], &user_io, sizeof(user_io)) != 0){
                rc = -1;
            }

            kfree_secure(kout, user_io.out_cap);
            kfree_secure(kin, user_io.in_len);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TLS_IS_READY: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_is_ready(pid, (int)frame[TF_X0]);
            return frame_sp;
        }

        case SYS_REMOTE_LOGIN_STATS:
            (void)net_poll();
            remote_login_poll();
            remote_login_dump_stats();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_REMOTE_LOGIN_STATE:
            frame[TF_X0] = (unsigned long)remote_login_state_bits();
            return frame_sp;

        case SYS_NET_GET_LOCAL_IP:
        {
            unsigned char ip[4];
            net_proto_get_local_ip(ip);
            frame[TF_X0] = (process_copy_to_user((void*)frame[TF_X0], ip, sizeof(ip)) == 0)
                               ? 0ul
                               : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_NET_SET_LOCAL_IP:
        {
            unsigned char ip[4];
            if (process_copy_from_user(ip, (const void*)frame[TF_X0], sizeof(ip)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            net_proto_set_local_ip(ip);
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_NET_GET_GATEWAY_IP:
        {
            unsigned char ip[4];
            net_proto_get_gateway_ip(ip);
            frame[TF_X0] = (process_copy_to_user((void*)frame[TF_X0], ip, sizeof(ip)) == 0)
                               ? 0ul
                               : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_NET_SET_GATEWAY_IP:
        {
            unsigned char ip[4];
            if (process_copy_from_user(ip, (const void*)frame[TF_X0], sizeof(ip)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            net_proto_set_gateway_ip(ip);
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_AUTH_IS_READY:
            frame[TF_X0] = (unsigned long)auth_is_ready();
            return frame_sp;

        case SYS_AUTH_GET_USERNAME:
        {
            char kname[AUTH_USERNAME_MAX + 1u];
            unsigned int user_cap = (unsigned int)frame[TF_X1];
            unsigned int copy_len = 0u;

            if (!auth_is_ready()){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (user_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(kname); i++){
                kname[i] = 0;
            }
            if (copy_cstr_out(kname, (unsigned int)sizeof(kname), auth_username()) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            copy_len = cstr_bytes_with_nul(kname, (unsigned int)sizeof(kname));
            if (copy_len == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (user_cap < copy_len){
                copy_len = user_cap;
                kname[copy_len - 1u] = 0;
            }
            frame[TF_X0] = (process_copy_to_user((void*)frame[TF_X0], kname, copy_len) == 0)
                               ? 0ul
                               : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_AUTH_VERIFY_PASSWORD:
        {
            char username[AUTH_USERNAME_MAX + 1u];
            char password[USER_PASS_MAX];
            int auth_rc = -1;
            if (copy_cstr_from_user_bound(username, sizeof(username), (const char*)frame[TF_X0]) != 0 ||
                copy_cstr_from_user_bound(password, sizeof(password), (const char*)frame[TF_X1]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            auth_rc = auth_verify_password(username, password);
            if (auth_rc == 0){
                headless_control_note_login_success();
            }
            for (unsigned int i = 0; i < sizeof(password); i++){
                password[i] = 0;
            }
            for (unsigned int i = 0; i < sizeof(username); i++){
                username[i] = 0;
            }
            frame[TF_X0] = (unsigned long)auth_rc;
            return frame_sp;
        }

        case SYS_WIFI_INIT:
            frame[TF_X0] = (unsigned long)cyw43_init();
            return frame_sp;

        case SYS_WIFI_LOAD_FW:
        {
            char fw_name[32];
            char nv_name[32];
            char clm_name[32];
            const char* fw_arg = 0;
            const char* nv_arg = 0;
            const char* clm_arg = 0;

            if ((const void*)frame[TF_X0]){
                if (copy_cstr_from_user_bound(fw_name, sizeof(fw_name), (const char*)frame[TF_X0]) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
                fw_arg = fw_name;
            }
            if ((const void*)frame[TF_X1]){
                if (copy_cstr_from_user_bound(nv_name, sizeof(nv_name), (const char*)frame[TF_X1]) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
                nv_arg = nv_name;
            }
            if ((const void*)frame[TF_X2]){
                if (copy_cstr_from_user_bound(clm_name, sizeof(clm_name), (const char*)frame[TF_X2]) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
                clm_arg = clm_name;
            }
            kernel_preempt_enter();
            frame[TF_X0] = (unsigned long)cyw43_upload_firmware_from_fat(fw_arg, nv_arg, clm_arg);
            kernel_preempt_exit();
            return frame_sp;
        }

        case SYS_WIFI_UP:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_up();
            return frame_sp;

        case SYS_WIFI_UP_MONITOR:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_up_monitor();
            return frame_sp;

        case SYS_WIFI_RANDOMIZE_MAC:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_randomize_mac();
            return frame_sp;

        case SYS_LED_TEST:
            frame[TF_X0] = (unsigned long)headless_led_test((unsigned int)frame[TF_X0],
                                                            (unsigned int)frame[TF_X1],
                                                            (unsigned int)frame[TF_X2]);
            return frame_sp;

        case SYS_LED_SET:
            frame[TF_X0] = (unsigned long)headless_led_force((unsigned int)frame[TF_X0]);
            return frame_sp;

        case SYS_LED_STATUS:
            frame[TF_X0] = (unsigned long)headless_led_status_word();
            return frame_sp;

        case SYS_HEADLESS_OPEN_HIT:
        {
            char sandbox83[11];
            static const char scanner83[11] = {'S','C','A','N','N','E','R',' ',' ',' ',' '};
            int ok = 1;
            if (process_current_file_sandbox(sandbox83) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < 11u; i++){
                if (sandbox83[i] != scanner83[i]){
                    ok = 0;
                    break;
                }
            }
            if (!ok){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            headless_control_note_open_network_packet();
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_HEADLESS_SCANNER_IDLE:
        {
            char sandbox83[11];
            static const char scanner83[11] = {'S','C','A','N','N','E','R',' ',' ',' ',' '};
            int ok = 1;
            if (process_current_file_sandbox(sandbox83) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < 11u; i++){
                if (sandbox83[i] != scanner83[i]){
                    ok = 0;
                    break;
                }
            }
            if (!ok){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            headless_control_note_scanner_idle((unsigned int)frame[TF_X0] ? 1u : 0u);
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_HEADLESS_PROBE_PAUSE_ACTIVE:
        {
            frame[TF_X0] = (unsigned long)headless_control_probe_pause_active();
            return frame_sp;
        }

        case SYS_SCANNER_LOG_SET_PASSWORD:
        {
            char password[USER_PASS_MAX];
            if (!frame[TF_X0] ||
                copy_cstr_from_user_bound(password, sizeof(password), (const char*)frame[TF_X0]) != 0){
                scanner_log_clear_password();
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (scanner_log_set_password_kernel(password) == 0) ? 0ul : (unsigned long)-1;
            crypto_memzero(password, sizeof(password));
            return frame_sp;
        }

        case SYS_SCANNER_LOG_READ:
        {
            static const char scanner83[11] = {'S','C','A','N','N','E','R',' ',' ',' ',' '};
            char path[USER_BMP_PATH_MAX];
            unsigned int offset = (unsigned int)frame[TF_X1];
            unsigned char* user_out = (unsigned char*)frame[TF_X2];
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X3], USER_FILE_RW_MAX);
            unsigned char* kbuf = 0;
            int n = -1;

            if (!frame[TF_X0] || !user_out || out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(path); i++){
                path[i] = 0;
            }
            if (process_copy_cstr_from_user(path, sizeof(path), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kbuf = (unsigned char*)kmalloc((unsigned long)out_cap);
            if (!kbuf){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            n = sandbox_file_read_at(scanner83, path, offset, kbuf, out_cap);
            if (n > 0 && process_copy_to_user(user_out, kbuf, (unsigned long)n) != 0){
                kfree_secure(kbuf, out_cap);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kfree_secure(kbuf, out_cap);
            frame[TF_X0] = (n >= 0) ? (unsigned long)n : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_SCANNER_LOG_FLUSH:
        {
            static const char scanner83[11] = {'S','C','A','N','N','E','R',' ',' ',' ',' '};
            char path[USER_BMP_PATH_MAX];
            int rc = -1;

            if (!frame[TF_X0]){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(path); i++){
                path[i] = 0;
            }
            if (process_copy_cstr_from_user(path, sizeof(path), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kernel_preempt_enter();
            /*
             * Scanner monitor mode can reserve the shared EMMC/SDIO path.
             * Save through EMMC only; SDHOST is intentionally not used for
             * scanner persistence.
             */
            if (scanner_fat_prepare_emmc() == 0 && fat32_init() == 0){
                rc = scanner_log_flush_to_fat_encrypted(scanner83, path, scanner_log_is_replace_file(path));
            }
            if (rc != 0 && blockdev_reinit_emmc_from_wifi() == 0 &&
                blockdev_is_emmc() && fat32_init() == 0){
                rc = scanner_log_flush_to_fat_encrypted(scanner83, path, scanner_log_is_replace_file(path));
            }
            kernel_preempt_exit();

            frame[TF_X0] = (rc == 0) ? 0ul : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_SCANNER_LOG_FLUSH_ALL:
        {
            int rc = scanner_log_flush_all_to_fat_kernel();
            frame[TF_X0] = (rc == 0) ? 0ul : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_SCANNER_LOG_READ_FAT:
        {
            static const char scanner83[11] = {'S','C','A','N','N','E','R',' ',' ',' ',' '};
            char path[USER_BMP_PATH_MAX];
            unsigned int offset = (unsigned int)frame[TF_X1];
            unsigned char* user_out = (unsigned char*)frame[TF_X2];
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X3], USER_FILE_RW_MAX);
            unsigned char* kbuf = 0;
            int n = -1;

            if (!frame[TF_X0] || !user_out || out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(path); i++){
                path[i] = 0;
            }
            if (process_copy_cstr_from_user(path, sizeof(path), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kbuf = (unsigned char*)kmalloc((unsigned long)out_cap);
            if (!kbuf){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kernel_preempt_enter();
            if (scanner_fat_prepare_emmc() == 0 && fat32_init() == 0){
                n = scanner_log_read_fat_plaintext(scanner83, path, offset, kbuf, out_cap);
            }
            if (n < 0 && blockdev_reinit_emmc_from_wifi() == 0 &&
                blockdev_is_emmc() && fat32_init() == 0){
                n = scanner_log_read_fat_plaintext(scanner83, path, offset, kbuf, out_cap);
            }
            kernel_preempt_exit();

            if (n > 0 && process_copy_to_user(user_out, kbuf, (unsigned long)n) != 0){
                kfree_secure(kbuf, out_cap);
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kfree_secure(kbuf, out_cap);
            frame[TF_X0] = (n >= 0) ? (unsigned long)n : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_WIFI_DOWN:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_down();
            return frame_sp;

        case SYS_WIFI_SCAN: {
            cyw43_scan_result_t* user_out = (cyw43_scan_result_t*)frame[TF_X0];
            unsigned int cap = clamp_u32((unsigned int)frame[TF_X1], USER_WIFI_SCAN_MAX);
            cyw43_scan_result_t* kout = 0;
            unsigned int count = 0;
            int rc = -1;

            if (!user_out || cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kout = (cyw43_scan_result_t*)kmalloc((unsigned long)sizeof(cyw43_scan_result_t) * cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            rc = cyw43_ioctl_scan(kout, cap, &count);
            if (rc == 0){
                if (count > cap){
                    count = cap;
                }
                if (count > 0u &&
                    process_copy_to_user(user_out,
                                         kout,
                                         (unsigned long)sizeof(cyw43_scan_result_t) * count) != 0){
                    rc = -1;
                }
            }
            kfree_secure(kout, (unsigned long)sizeof(cyw43_scan_result_t) * cap);
            frame[TF_X0] = (rc == 0) ? (unsigned long)count : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_WIFI_SCAN_SSID: {
            char ssid[33];
            cyw43_scan_result_t* user_out = (cyw43_scan_result_t*)frame[TF_X1];
            unsigned int cap = clamp_u32((unsigned int)frame[TF_X2], USER_WIFI_SCAN_MAX);
            cyw43_scan_result_t* kout = 0;
            unsigned int count = 0;
            int rc = -1;

            if (copy_cstr_from_user_bound(ssid, sizeof(ssid), (const char*)frame[TF_X0]) != 0 ||
                !user_out || cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kout = (cyw43_scan_result_t*)kmalloc((unsigned long)sizeof(cyw43_scan_result_t) * cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            rc = cyw43_ioctl_scan_ssid(ssid, kout, cap, &count);
            if (rc == 0){
                if (count > cap){
                    count = cap;
                }
                if (count > 0u &&
                    process_copy_to_user(user_out,
                                         kout,
                                         (unsigned long)sizeof(cyw43_scan_result_t) * count) != 0){
                    rc = -1;
                }
            }
            for (unsigned int i = 0; i < sizeof(ssid); i++){
                ssid[i] = 0;
            }
            kfree_secure(kout, (unsigned long)sizeof(cyw43_scan_result_t) * cap);
            frame[TF_X0] = (rc == 0) ? (unsigned long)count : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_WIFI_RAW_SET_ENABLED:
            frame[TF_X0] = (unsigned long)cyw43_raw_capture_set_enabled((unsigned int)frame[TF_X0]);
            return frame_sp;

        case SYS_WIFI_RAW_RECV:
        {
            unsigned char* user_out = (unsigned char*)frame[TF_X0];
            unsigned int cap = clamp_u32((unsigned int)frame[TF_X1], 2304u);
            unsigned char* kbuf = 0;
            int n = 0;
            if (!user_out || cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            (void)cyw43_raw_capture_poll();
            kbuf = (unsigned char*)kmalloc(cap);
            if (!kbuf){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            n = cyw43_raw_capture_recv(kbuf, cap);
            if (n > 0){
                if (process_copy_to_user(user_out, kbuf, (unsigned long)n) != 0){
                    kfree_secure(kbuf, cap);
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
            }
            kfree_secure(kbuf, cap);
            frame[TF_X0] = (unsigned long)n;
            return frame_sp;
        }

        case SYS_WIFI_RAW_STATUS:
        {
            cyw43_raw_capture_status_t* user_out = (cyw43_raw_capture_status_t*)frame[TF_X0];
            cyw43_raw_capture_status_t kout;
            (void)cyw43_raw_capture_poll();
            if (!user_out || cyw43_raw_capture_get_status(&kout) != 0 ||
                process_copy_to_user(user_out, &kout, sizeof(kout)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_WIFI_MONITOR_SET:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_monitor((unsigned int)frame[TF_X0],
                                                              (unsigned int)frame[TF_X1]);
            return frame_sp;

        case SYS_WIFI_MONITOR_STATUS:
        {
            cyw43_monitor_status_t* user_out = (cyw43_monitor_status_t*)frame[TF_X0];
            cyw43_monitor_status_t kout;
            if (!user_out || cyw43_ioctl_monitor_status(&kout) != 0 ||
                process_copy_to_user(user_out, &kout, sizeof(kout)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_WIFI_MONITOR_RECOVER:
            kernel_preempt_enter();
            frame[TF_X0] = (unsigned long)cyw43_monitor_hard_recover((unsigned int)frame[TF_X0]);
            kernel_preempt_exit();
            return frame_sp;

        case SYS_WIFI_JOIN:
        {
            char ssid[33];
            char password[USER_PASS_MAX];
            const char* pass_arg = 0;
            int join_rc = -1;
            if (copy_cstr_from_user_bound(ssid, sizeof(ssid), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if ((const void*)frame[TF_X1]){
                if (copy_cstr_from_user_bound(password, sizeof(password), (const char*)frame[TF_X1]) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
                pass_arg = password;
            }
            join_rc = cyw43_ioctl_join(ssid, pass_arg);
            for (unsigned int i = 0; i < sizeof(password); i++){
                password[i] = 0;
            }
            for (unsigned int i = 0; i < sizeof(ssid); i++){
                ssid[i] = 0;
            }
            frame[TF_X0] = (unsigned long)join_rc;
            return frame_sp;
        }

        case SYS_WIFI_DUMP_STATUS:
            cyw43_dump_status();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_WIFI_GET_VERSION:
        {
            char* user_out = (char*)frame[TF_X0];
            unsigned int user_cap = clamp_u32((unsigned int)frame[TF_X1], USER_CSTR_MAX);
            char* kout = 0;
            int rc = -1;

            if (!user_out || user_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kout = (char*)kmalloc(user_cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            rc = cyw43_get_firmware_version(kout, user_cap);
            if (rc == 0){
                unsigned int copy_len = cstr_bytes_with_nul(kout, user_cap);
                if (copy_len == 0u){
                    copy_len = 1u;
                }
                if (process_copy_to_user(user_out, kout, copy_len) != 0){
                    rc = -1;
                }
            }
            kfree_secure(kout, user_cap);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        default:
            frame[TF_X0] = (unsigned long)-1;
            return frame_sp;
    }
}
