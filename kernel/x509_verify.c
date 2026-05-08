#include "x509_verify.h"
#include "fat32.h"
#include "sha256.h"
#include "rsa_verify.h"
#include "trust.h"
#include "program.h"
#include "pq_sig.h"
#include "ed25519_verify.h"
#include "crypto.h"
#include "string.h"
#include "uart.h"
#include "timer.h"

#define X509_MAX_CHAIN_CERTS 8u
#define X509_MAX_CA_ANCHORS 256u
#define X509_CA_PEM_MAX (512u * 1024u)
#define X509_CA_SIG_MAX 128u
#define X509_CA_LABEL "CA_ROOTS_PEM"
#define X509_CA_LABEL_LEN 12u
#define X509_REVOKE_LABEL "X509_REVOKE"
#define X509_REVOKE_LABEL_LEN 11u
#define X509_REVOKE_BIN_FAT "X509REVOKBIN"
#define X509_REVOKE_SIG_FAT "X509REVOKSIG"
#define X509_REVOKE_PQS_FAT "X509REVOKPQS"
#define X509_REVOKE_BIN_MAX (256u * 1024u)
#define X509_REVOKE_SIG_MAX 128u
#define X509_REVOKE_MAX_ENTRIES 2048u
#define X509_NC_DNS_MAX 16u

#define X509_KU_DIGITAL_SIGNATURE (1u << 0)
#define X509_KU_KEY_CERT_SIGN     (1u << 5)

// FAT directory entries are raw 8.3 bytes with no dot. CA_ROOTS is already
// 8 chars, so the extension begins immediately at byte 8.
#define X509_CA_PEM_FAT "CA_ROOTSPEM"
#define X509_CA_SIG_FAT "CA_ROOTSSIG"
#define X509_CA_PQS_FAT "CA_ROOTSPQS"
#define X509_KEY_ALG_NONE 0u
#define X509_KEY_ALG_RSA 1u
#define X509_KEY_ALG_EC 2u
#define X509_EC_COORD_MAX 48u

typedef struct {
    unsigned char tag;
    const unsigned char* hdr;
    unsigned int hdr_len;
    const unsigned char* val;
    unsigned int val_len;
    unsigned int total_len;
} asn1_tlv_t;

typedef struct {
    const unsigned char* subject_tlv;
    unsigned int subject_tlv_len;
    const unsigned char* issuer_tlv;
    unsigned int issuer_tlv_len;
    const unsigned char* serial_bytes;
    unsigned int serial_len;
    const unsigned char* tbs_tlv;
    unsigned int tbs_tlv_len;
    const unsigned char* spki_tlv;
    unsigned int spki_tlv_len;
    const unsigned char* sig_bits;
    unsigned int sig_bits_len;
    unsigned short cert_sig_alg;
    unsigned int key_alg;
    unsigned char rsa_n[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned int rsa_n_len;
    unsigned char rsa_e[RSA_VERIFY_MAX_EXP_BYTES];
    unsigned int rsa_e_len;
    unsigned int ec_curve;
    unsigned char ec_qx[X509_EC_COORD_MAX];
    unsigned char ec_qy[X509_EC_COORD_MAX];
    unsigned int ec_q_len;
    int basic_constraints_present;
    int is_ca;
    int path_len_present;
    unsigned int path_len_constraint;
    int key_usage_present;
    unsigned int key_usage_bits;
    int cert_policies_present;
    int cert_policy_any;
    int policy_mappings_present;
    int policy_constraints_req_exp_present;
    unsigned int policy_constraints_req_exp;
    int policy_constraints_inhibit_map_present;
    unsigned int policy_constraints_inhibit_map;
    int inhibit_any_policy_present;
    unsigned int inhibit_any_policy_skip;
    int name_constraints_present;
    int name_constraints_critical;
    int name_constraints_parse_error;
    unsigned int nc_dns_permit_count;
    const unsigned char* nc_dns_permit[X509_NC_DNS_MAX];
    unsigned int nc_dns_permit_len[X509_NC_DNS_MAX];
    unsigned int nc_dns_exclude_count;
    const unsigned char* nc_dns_exclude[X509_NC_DNS_MAX];
    unsigned int nc_dns_exclude_len[X509_NC_DNS_MAX];
    int eku_present;
    int eku_server_auth;
    int eku_any;
    int unknown_critical_ext;
    int validity_present;
    long long not_before_unix;
    long long not_after_unix;
    int san_present;
    int hostname_match;
} parsed_cert_t;

typedef struct {
    unsigned char issuer_hash[32];
    unsigned char serial_hash[32];
} revoked_cert_t;

typedef struct {
    unsigned char subject_hash[32];
    unsigned char der_hash[32];
    unsigned int key_alg;
    unsigned char rsa_n[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned int rsa_n_len;
    unsigned char rsa_e[RSA_VERIFY_MAX_EXP_BYTES];
    unsigned int rsa_e_len;
    unsigned int ec_curve;
    unsigned char ec_qx[X509_EC_COORD_MAX];
    unsigned char ec_qy[X509_EC_COORD_MAX];
    unsigned int ec_q_len;
    int basic_constraints_present;
    int is_ca;
    int path_len_present;
    unsigned int path_len_constraint;
    int key_usage_present;
    unsigned int key_usage_bits;
} ca_anchor_t;

static unsigned char g_ca_pem_buf[X509_CA_PEM_MAX + 1u];
static unsigned char g_ca_sig_buf[X509_CA_SIG_MAX];
static unsigned char g_ca_pqs_buf[QOS_PQ_SIG_MAX];
static unsigned char g_ca_der_tmp[8192];
static unsigned char g_revoke_bin_buf[X509_REVOKE_BIN_MAX];
static unsigned char g_revoke_sig_buf[X509_REVOKE_SIG_MAX];
static unsigned char g_revoke_pqs_buf[QOS_PQ_SIG_MAX];
static ca_anchor_t g_ca_anchors[X509_MAX_CA_ANCHORS];
static revoked_cert_t g_revoked[X509_REVOKE_MAX_ENTRIES];
static unsigned int g_ca_anchor_count = 0u;
static unsigned int g_revoked_count = 0u;
static int g_ca_cache_state = 0; // 0=uninitialized, 1=ready, -1=failed
static int g_revoke_cache_state = 0; // 0=uninitialized, 1=ready, 2=missing, -1=failed
static int g_require_revocation_list = 0;
static long long g_validation_time_unix = 0;
static int g_validation_time_set = 0;
static int g_validation_time_source = 0; // 0=none, 1=build-fallback, 2=external
static int g_require_explicit_validation_time = 0;

static unsigned int get_u32_le(const unsigned char* p){
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static void put_u32_le(unsigned char* out, unsigned int v){
    out[0] = (unsigned char)(v & 0xFFu);
    out[1] = (unsigned char)((v >> 8) & 0xFFu);
    out[2] = (unsigned char)((v >> 16) & 0xFFu);
    out[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put_u64_le(unsigned char* out, unsigned long long v){
    out[0] = (unsigned char)(v & 0xFFull);
    out[1] = (unsigned char)((v >> 8) & 0xFFull);
    out[2] = (unsigned char)((v >> 16) & 0xFFull);
    out[3] = (unsigned char)((v >> 24) & 0xFFull);
    out[4] = (unsigned char)((v >> 32) & 0xFFull);
    out[5] = (unsigned char)((v >> 40) & 0xFFull);
    out[6] = (unsigned char)((v >> 48) & 0xFFull);
    out[7] = (unsigned char)((v >> 56) & 0xFFull);
}

static unsigned int be24_read(const unsigned char* p){
    return ((unsigned int)p[0] << 16) |
           ((unsigned int)p[1] << 8) |
           (unsigned int)p[2];
}

static int parse_dec_n(const unsigned char* s, unsigned int n, int* out){
    int v = 0;
    if (!s || !out || n == 0u){
        return -1;
    }
    for (unsigned int i = 0; i < n; i++){
        unsigned char c = s[i];
        if (c < '0' || c > '9'){
            return -1;
        }
        v = (v * 10) + (int)(c - '0');
    }
    *out = v;
    return 0;
}

static int is_leap_year(int y){
    if ((y % 4) != 0){
        return 0;
    }
    if ((y % 100) != 0){
        return 1;
    }
    return (y % 400) == 0;
}

static int days_in_month(int y, int m){
    static const int dm[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12){
        return 0;
    }
    if (m == 2 && is_leap_year(y)){
        return 29;
    }
    return dm[m - 1];
}

static int ymdhms_to_unix(int y, int mon, int day, int hh, int mm, int ss, long long* out){
    long long days = 0;
    if (!out){
        return -1;
    }
    if (y < 1970 || mon < 1 || mon > 12 || day < 1 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60){
        return -1;
    }
    if (day > days_in_month(y, mon)){
        return -1;
    }
    for (int yr = 1970; yr < y; yr++){
        days += is_leap_year(yr) ? 366 : 365;
    }
    for (int m = 1; m < mon; m++){
        days += days_in_month(y, m);
    }
    days += (long long)(day - 1);
    *out = (((days * 24ll) + (long long)hh) * 60ll + (long long)mm) * 60ll + (long long)ss;
    return 0;
}

static int parse_asn1_time_to_unix(const asn1_tlv_t* t, long long* out_unix){
    int y = 0;
    int mon = 0;
    int day = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    if (!t || !out_unix){
        return -1;
    }
    // RFC 5280 profile uses UTC time (tag 0x17, YYMMDDHHMMSSZ) and
    // generalized time (tag 0x18, YYYYMMDDHHMMSSZ).
    if (t->tag == 0x17u){
        int yy = 0;
        if (t->val_len != 13u || t->val[12] != 'Z'){
            return -1;
        }
        if (parse_dec_n(t->val + 0u, 2u, &yy) != 0 ||
            parse_dec_n(t->val + 2u, 2u, &mon) != 0 ||
            parse_dec_n(t->val + 4u, 2u, &day) != 0 ||
            parse_dec_n(t->val + 6u, 2u, &hh) != 0 ||
            parse_dec_n(t->val + 8u, 2u, &mm) != 0 ||
            parse_dec_n(t->val + 10u, 2u, &ss) != 0){
            return -1;
        }
        y = (yy >= 50) ? (1900 + yy) : (2000 + yy);
        return ymdhms_to_unix(y, mon, day, hh, mm, ss, out_unix);
    }
    if (t->tag == 0x18u){
        if (t->val_len != 15u || t->val[14] != 'Z'){
            return -1;
        }
        if (parse_dec_n(t->val + 0u, 4u, &y) != 0 ||
            parse_dec_n(t->val + 4u, 2u, &mon) != 0 ||
            parse_dec_n(t->val + 6u, 2u, &day) != 0 ||
            parse_dec_n(t->val + 8u, 2u, &hh) != 0 ||
            parse_dec_n(t->val + 10u, 2u, &mm) != 0 ||
            parse_dec_n(t->val + 12u, 2u, &ss) != 0){
            return -1;
        }
        return ymdhms_to_unix(y, mon, day, hh, mm, ss, out_unix);
    }
    return -1;
}

static int month_from_build(const char* m3){
    if (!m3){
        return 0;
    }
    if (m3[0] == 'J' && m3[1] == 'a' && m3[2] == 'n') return 1;
    if (m3[0] == 'F' && m3[1] == 'e' && m3[2] == 'b') return 2;
    if (m3[0] == 'M' && m3[1] == 'a' && m3[2] == 'r') return 3;
    if (m3[0] == 'A' && m3[1] == 'p' && m3[2] == 'r') return 4;
    if (m3[0] == 'M' && m3[1] == 'a' && m3[2] == 'y') return 5;
    if (m3[0] == 'J' && m3[1] == 'u' && m3[2] == 'n') return 6;
    if (m3[0] == 'J' && m3[1] == 'u' && m3[2] == 'l') return 7;
    if (m3[0] == 'A' && m3[1] == 'u' && m3[2] == 'g') return 8;
    if (m3[0] == 'S' && m3[1] == 'e' && m3[2] == 'p') return 9;
    if (m3[0] == 'O' && m3[1] == 'c' && m3[2] == 't') return 10;
    if (m3[0] == 'N' && m3[1] == 'o' && m3[2] == 'v') return 11;
    if (m3[0] == 'D' && m3[1] == 'e' && m3[2] == 'c') return 12;
    return 0;
}

static void seed_validation_time_from_build_if_unset(void){
    int mon = 0;
    int day = 0;
    int year = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    long long unix_time = 0;
    const char* d = __DATE__; // "Mmm dd yyyy"
    const char* t = __TIME__; // "hh:mm:ss"

    if (g_validation_time_set){
        return;
    }

    mon = month_from_build(d);
    if (mon == 0){
        return;
    }
    if (d[4] == ' '){
        day = d[5] - '0';
    } else{
        day = ((d[4] - '0') * 10) + (d[5] - '0');
    }
    year = ((d[7] - '0') * 1000) + ((d[8] - '0') * 100) + ((d[9] - '0') * 10) + (d[10] - '0');
    hh = ((t[0] - '0') * 10) + (t[1] - '0');
    mm = ((t[3] - '0') * 10) + (t[4] - '0');
    ss = ((t[6] - '0') * 10) + (t[7] - '0');
    if (ymdhms_to_unix(year, mon, day, hh, mm, ss, &unix_time) == 0){
        g_validation_time_unix = unix_time;
        g_validation_time_set = 1;
        g_validation_time_source = 1;
    }
}

static unsigned char ascii_lower(unsigned char c){
    if (c >= 'A' && c <= 'Z'){
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static int ascii_equal_nocase(const char* a, unsigned int a_len,
                              const unsigned char* b, unsigned int b_len){
    if (a_len != b_len){
        return 0;
    }
    for (unsigned int i = 0; i < a_len; i++){
        if (ascii_lower((unsigned char)a[i]) != ascii_lower(b[i])){
            return 0;
        }
    }
    return 1;
}

static int trim_trailing_dot_len(const char* s, unsigned int len){
    while (len > 0u && s[len - 1u] == '.'){
        len--;
    }
    return (int)len;
}

static int trim_trailing_dot_u8_len(const unsigned char* s, unsigned int len){
    while (len > 0u && s[len - 1u] == '.'){
        len--;
    }
    return (int)len;
}

static int ascii_equal_nocase_u8(const char* a, unsigned int a_len,
                                 const unsigned char* b, unsigned int b_len){
    if (!a || !b || a_len != b_len){
        return 0;
    }
    for (unsigned int i = 0; i < a_len; i++){
        if (ascii_lower((unsigned char)a[i]) != ascii_lower(b[i])){
            return 0;
        }
    }
    return 1;
}

// RFC 5280 label-by-label suffix matching for dNSName subtrees.
static int dns_subtree_match(const char* host, unsigned int host_len,
                             const unsigned char* subtree, unsigned int subtree_len){
    if (!host || !subtree || host_len == 0u || subtree_len == 0u){
        return 0;
    }
    host_len = (unsigned int)trim_trailing_dot_len(host, host_len);
    subtree_len = (unsigned int)trim_trailing_dot_u8_len(subtree, subtree_len);
    if (host_len == 0u || subtree_len == 0u){
        return 0;
    }

    if (subtree[0] == '.'){
        // .example.com => subdomains only, not exact example.com
        subtree++;
        subtree_len--;
        if (subtree_len == 0u || host_len <= subtree_len){
            return 0;
        }
        if (!ascii_equal_nocase_u8(&host[host_len - subtree_len], subtree_len, subtree, subtree_len)){
            return 0;
        }
        return host[host_len - subtree_len - 1u] == '.';
    }

    // host.example.com => exact match OR any additional labels on the left.
    if (host_len == subtree_len){
        return ascii_equal_nocase_u8(host, host_len, subtree, subtree_len);
    }
    if (host_len > subtree_len){
        if (!ascii_equal_nocase_u8(&host[host_len - subtree_len], subtree_len, subtree, subtree_len)){
            return 0;
        }
        return host[host_len - subtree_len - 1u] == '.';
    }
    return 0;
}

static int hostname_pattern_match(const char* host, unsigned int host_len,
                                  const unsigned char* pat, unsigned int pat_len){
    if (!host || !pat || host_len == 0u || pat_len == 0u){
        return 0;
    }

    while (pat_len > 0u && pat[pat_len - 1u] == '.'){
        pat_len--;
    }
    while (host_len > 0u && host[host_len - 1u] == '.'){
        host_len--;
    }
    if (pat_len == 0u || host_len == 0u){
        return 0;
    }

    // Wildcard only allowed as "*.example.com" and only matches a single label.
    if (pat_len >= 3u && pat[0] == '*' && pat[1] == '.'){
        unsigned int dot = 0u;
        while (dot < host_len && host[dot] != '.'){
            dot++;
        }
        if (dot == 0u || dot >= host_len){
            return 0;
        }
        unsigned int suffix_len = pat_len - 1u;
        const unsigned char* suffix = &pat[1];
        if ((host_len - dot) != suffix_len){
            return 0;
        }
        for (unsigned int i = 0; i < suffix_len; i++){
            if (ascii_lower((unsigned char)host[dot + i]) != ascii_lower(suffix[i])){
                return 0;
            }
        }
        return 1;
    }

    return ascii_equal_nocase(host, host_len, pat, pat_len);
}

static int asn1_parse_tlv(const unsigned char* p, unsigned int len, asn1_tlv_t* out){
    if (!p || !out || len < 2u){
        return -1;
    }

    unsigned int off = 0u;
    unsigned char tag = p[off++];
    if (off >= len){
        return -1;
    }
    unsigned char lb = p[off++];
    unsigned int vlen = 0u;

    if ((lb & 0x80u) == 0u){
        vlen = (unsigned int)lb;
    } else{
        unsigned int n = (unsigned int)(lb & 0x7Fu);
        if (n == 0u || n > 4u || (off + n) > len){
            return -1;
        }
        for (unsigned int i = 0; i < n; i++){
            vlen = (vlen << 8) | p[off++];
        }
        if (vlen < 128u){
            return -1; // non-minimal DER length
        }
    }

    if ((off + vlen) > len){
        return -1;
    }

    out->tag = tag;
    out->hdr = p;
    out->hdr_len = off;
    out->val = p + off;
    out->val_len = vlen;
    out->total_len = off + vlen;
    return 0;
}

static int oid_equal(const asn1_tlv_t* oid,
                     const unsigned char* bytes,
                     unsigned int bytes_len){
    if (!oid || oid->tag != 0x06u || !bytes){
        return 0;
    }
    if (oid->val_len != bytes_len){
        return 0;
    }
    return memcmp(oid->val, bytes, bytes_len) == 0;
}

static int asn1_integer_positive_bytes(const asn1_tlv_t* i,
                                       const unsigned char** out,
                                       unsigned int* out_len){
    const unsigned char* p;
    unsigned int n;
    if (!i || !out || !out_len || i->tag != 0x02u || i->val_len == 0u){
        return -1;
    }
    if (i->val[0] & 0x80u){
        return -1;
    }
    p = i->val;
    n = i->val_len;
    if (n > 1u && p[0] == 0x00u){
        p++;
        n--;
    }
    if (n == 0u){
        return -1;
    }
    *out = p;
    *out_len = n;
    return 0;
}

static int copy_bytes_bounded(unsigned char* dst,
                              unsigned int dst_cap,
                              unsigned int* out_len,
                              const unsigned char* src,
                              unsigned int src_len){
    if (!dst || !out_len || !src || src_len == 0u || src_len > dst_cap){
        return -1;
    }
    for (unsigned int i = 0; i < src_len; i++){
        dst[i] = src[i];
    }
    *out_len = src_len;
    return 0;
}

static int parse_rsa_spki(const unsigned char* spki_tlv,
                          unsigned int spki_tlv_len,
                          unsigned char* out_n,
                          unsigned int* out_n_len,
                          unsigned char* out_e,
                          unsigned int* out_e_len){
    static const unsigned char OID_RSA_ENCRYPTION[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01 };
    static const unsigned char OID_RSA_PSS[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0A };
    asn1_tlv_t spki;
    asn1_tlv_t algid;
    asn1_tlv_t oid;
    asn1_tlv_t pubbits;
    asn1_tlv_t rsapk;
    asn1_tlv_t nint;
    asn1_tlv_t eint;
    const unsigned char* n_ptr;
    const unsigned char* e_ptr;
    unsigned int n_len;
    unsigned int e_len;
    unsigned int off = 0u;
    unsigned int roff = 0u;

    if (!spki_tlv || spki_tlv_len == 0u || !out_n || !out_n_len || !out_e || !out_e_len){
        return -1;
    }
    *out_n_len = 0u;
    *out_e_len = 0u;

    if (asn1_parse_tlv(spki_tlv, spki_tlv_len, &spki) != 0 || spki.tag != 0x30u){
        return -1;
    }
    if (asn1_parse_tlv(spki.val + off, spki.val_len - off, &algid) != 0 || algid.tag != 0x30u){
        return -1;
    }
    off += algid.total_len;
    if (asn1_parse_tlv(algid.val, algid.val_len, &oid) != 0 ||
        (!oid_equal(&oid, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION)) &&
         !oid_equal(&oid, OID_RSA_PSS, sizeof(OID_RSA_PSS)))){
        return -1;
    }
    if (asn1_parse_tlv(spki.val + off, spki.val_len - off, &pubbits) != 0 ||
        pubbits.tag != 0x03u || pubbits.val_len < 2u || pubbits.val[0] != 0u){
        return -1;
    }
    if (asn1_parse_tlv(pubbits.val + 1u, pubbits.val_len - 1u, &rsapk) != 0 || rsapk.tag != 0x30u){
        return -1;
    }
    if (asn1_parse_tlv(rsapk.val + roff, rsapk.val_len - roff, &nint) != 0){
        return -1;
    }
    roff += nint.total_len;
    if (asn1_parse_tlv(rsapk.val + roff, rsapk.val_len - roff, &eint) != 0){
        return -1;
    }
    if (asn1_integer_positive_bytes(&nint, &n_ptr, &n_len) != 0 ||
        asn1_integer_positive_bytes(&eint, &e_ptr, &e_len) != 0){
        return -1;
    }
    if (copy_bytes_bounded(out_n, RSA_VERIFY_MAX_MOD_BYTES, out_n_len, n_ptr, n_len) != 0 ||
        copy_bytes_bounded(out_e, RSA_VERIFY_MAX_EXP_BYTES, out_e_len, e_ptr, e_len) != 0){
        return -1;
    }
    return 0;
}

static int parse_ec_spki(const unsigned char* spki_tlv,
                         unsigned int spki_tlv_len,
                         unsigned int* out_curve,
                         unsigned char* out_qx,
                         unsigned char* out_qy,
                         unsigned int* out_q_len){
    static const unsigned char OID_ID_EC_PUBLIC_KEY[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01 };
    static const unsigned char OID_PRIME256V1[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07 };
    static const unsigned char OID_SECP384R1[] = { 0x2B,0x81,0x04,0x00,0x22 };
    asn1_tlv_t spki;
    asn1_tlv_t algid;
    asn1_tlv_t alg_oid;
    asn1_tlv_t curve_oid;
    asn1_tlv_t pubbits;
    unsigned int off = 0u;
    unsigned int q_len = 0u;
    unsigned int curve = ECDSA_VERIFY_CURVE_NONE;

    if (!spki_tlv || spki_tlv_len == 0u || !out_curve || !out_qx || !out_qy || !out_q_len){
        return -1;
    }
    *out_curve = ECDSA_VERIFY_CURVE_NONE;
    *out_q_len = 0u;

    if (asn1_parse_tlv(spki_tlv, spki_tlv_len, &spki) != 0 || spki.tag != 0x30u){
        return -1;
    }
    if (asn1_parse_tlv(spki.val + off, spki.val_len - off, &algid) != 0 || algid.tag != 0x30u){
        return -1;
    }
    off += algid.total_len;
    if (asn1_parse_tlv(algid.val, algid.val_len, &alg_oid) != 0 || alg_oid.tag != 0x06u){
        return -1;
    }
    if (!oid_equal(&alg_oid, OID_ID_EC_PUBLIC_KEY, sizeof(OID_ID_EC_PUBLIC_KEY))){
        return -1;
    }
    if (alg_oid.total_len >= algid.val_len){
        return -1;
    }
    if (asn1_parse_tlv(algid.val + alg_oid.total_len,
                       algid.val_len - alg_oid.total_len,
                       &curve_oid) != 0 || curve_oid.tag != 0x06u){
        return -1;
    }
    if (oid_equal(&curve_oid, OID_PRIME256V1, sizeof(OID_PRIME256V1))){
        curve = ECDSA_VERIFY_CURVE_P256;
        q_len = 32u;
    } else if (oid_equal(&curve_oid, OID_SECP384R1, sizeof(OID_SECP384R1))){
        curve = ECDSA_VERIFY_CURVE_P384;
        q_len = 48u;
    } else{
        return -1;
    }

    if (asn1_parse_tlv(spki.val + off, spki.val_len - off, &pubbits) != 0 ||
        pubbits.tag != 0x03u || pubbits.val_len < 2u || pubbits.val[0] != 0u){
        return -1;
    }
    if (pubbits.val[1] != 0x04u){
        return -1;
    }
    if (pubbits.val_len != (2u + (2u * q_len))){
        return -1;
    }
    for (unsigned int i = 0; i < q_len; i++){
        out_qx[i] = pubbits.val[2u + i];
        out_qy[i] = pubbits.val[2u + q_len + i];
    }
    *out_curve = curve;
    *out_q_len = q_len;
    return 0;
}

static int parse_alg_id_hash_to_sig(const asn1_tlv_t* algid, unsigned short* out_sig_alg){
    static const unsigned char OID_SHA256[] = { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01 };
    static const unsigned char OID_SHA384[] = { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02 };
    static const unsigned char OID_SHA512[] = { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03 };

    if (!algid || !out_sig_alg || algid->tag != 0x30u){
        return -1;
    }

    unsigned int off = 0u;
    asn1_tlv_t oid;
    if (asn1_parse_tlv(algid->val + off, algid->val_len - off, &oid) != 0 || oid.tag != 0x06u){
        return -1;
    }
    off += oid.total_len;

    if (oid_equal(&oid, OID_SHA256, sizeof(OID_SHA256))){
        *out_sig_alg = 0x0804u;
        return 0;
    }
    if (oid_equal(&oid, OID_SHA384, sizeof(OID_SHA384))){
        *out_sig_alg = 0x0805u;
        return 0;
    }
    if (oid_equal(&oid, OID_SHA512, sizeof(OID_SHA512))){
        *out_sig_alg = 0x0806u;
        return 0;
    }

    (void)off;
    return -1;
}

static int parse_signature_algorithm(const asn1_tlv_t* algid, unsigned short* out_sig_alg){
    static const unsigned char OID_RSA_SHA256[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B };
    static const unsigned char OID_RSA_SHA384[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C };
    static const unsigned char OID_RSA_SHA512[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D };
    static const unsigned char OID_RSA_PSS[]    = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0A };
    static const unsigned char OID_ECDSA_SHA256[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02 };
    static const unsigned char OID_ECDSA_SHA384[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03 };
    static const unsigned char OID_ECDSA_SHA512[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x04 };
    static const unsigned char OID_ED25519[] = { 0x2B,0x65,0x70 };

    if (!algid || !out_sig_alg || algid->tag != 0x30u){
        return -1;
    }

    unsigned int off = 0u;
    asn1_tlv_t oid;
    if (asn1_parse_tlv(algid->val + off, algid->val_len - off, &oid) != 0 || oid.tag != 0x06u){
        return -1;
    }
    off += oid.total_len;

    if (oid_equal(&oid, OID_RSA_SHA256, sizeof(OID_RSA_SHA256))){
        *out_sig_alg = 0x0401u;
        return 0;
    }
    if (oid_equal(&oid, OID_RSA_SHA384, sizeof(OID_RSA_SHA384))){
        *out_sig_alg = 0x0501u;
        return 0;
    }
    if (oid_equal(&oid, OID_RSA_SHA512, sizeof(OID_RSA_SHA512))){
        *out_sig_alg = 0x0601u;
        return 0;
    }
    if (oid_equal(&oid, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256))){
        *out_sig_alg = 0x0403u;
        return 0;
    }
    if (oid_equal(&oid, OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384))){
        *out_sig_alg = 0x0503u;
        return 0;
    }
    if (oid_equal(&oid, OID_ECDSA_SHA512, sizeof(OID_ECDSA_SHA512))){
        *out_sig_alg = 0x0603u;
        return 0;
    }
    if (oid_equal(&oid, OID_ED25519, sizeof(OID_ED25519))){
        *out_sig_alg = 0x0807u;
        return 0;
    }
    if (oid_equal(&oid, OID_RSA_PSS, sizeof(OID_RSA_PSS))){
        // Parse PSS params hashAlgorithm if present.
        if (off < algid->val_len){
            asn1_tlv_t params;
            if (asn1_parse_tlv(algid->val + off, algid->val_len - off, &params) == 0 &&
                params.tag == 0x30u){
                unsigned int poff = 0u;
                while (poff < params.val_len){
                    asn1_tlv_t fld;
                    if (asn1_parse_tlv(params.val + poff, params.val_len - poff, &fld) != 0){
                        break;
                    }
                    // hashAlgorithm [0]
                    if (fld.tag == 0xA0u){
                        asn1_tlv_t hash_alg;
                        if (asn1_parse_tlv(fld.val, fld.val_len, &hash_alg) == 0 &&
                            parse_alg_id_hash_to_sig(&hash_alg, out_sig_alg) == 0){
                            return 0;
                        }
                    }
                    poff += fld.total_len;
                }
            }
        }
        *out_sig_alg = 0x0804u; // default most common for TLS 1.3
        return 0;
    }
    return -1;
}

static int parse_subject_cn_match(const unsigned char* subject_tlv,
                                  unsigned int subject_tlv_len,
                                  const char* host,
                                  unsigned int host_len){
    static const unsigned char OID_CN[] = { 0x55,0x04,0x03 };
    asn1_tlv_t subject;
    if (!subject_tlv || subject_tlv_len == 0u || !host || host_len == 0u){
        return 0;
    }
    if (asn1_parse_tlv(subject_tlv, subject_tlv_len, &subject) != 0 || subject.tag != 0x30u){
        return 0;
    }

    unsigned int roff = 0u;
    while (roff < subject.val_len){
        asn1_tlv_t rdn_set;
        if (asn1_parse_tlv(subject.val + roff, subject.val_len - roff, &rdn_set) != 0 ||
            rdn_set.tag != 0x31u){
            return 0;
        }
        roff += rdn_set.total_len;

        unsigned int aoff = 0u;
        while (aoff < rdn_set.val_len){
            asn1_tlv_t atv;
            if (asn1_parse_tlv(rdn_set.val + aoff, rdn_set.val_len - aoff, &atv) != 0 ||
                atv.tag != 0x30u){
                return 0;
            }
            aoff += atv.total_len;

            unsigned int voff = 0u;
            asn1_tlv_t oid;
            if (asn1_parse_tlv(atv.val + voff, atv.val_len - voff, &oid) != 0 || oid.tag != 0x06u){
                continue;
            }
            voff += oid.total_len;
            if (voff >= atv.val_len){
                continue;
            }
            asn1_tlv_t val;
            if (asn1_parse_tlv(atv.val + voff, atv.val_len - voff, &val) != 0){
                continue;
            }
            if (oid_equal(&oid, OID_CN, sizeof(OID_CN))){
                return hostname_pattern_match(host, host_len, val.val, val.val_len);
            }
        }
    }
    return 0;
}

static int parse_san_dns_match(const unsigned char* ext_value_der,
                               unsigned int ext_value_der_len,
                               const char* host,
                               unsigned int host_len,
                               int* out_has_dns_san){
    if (out_has_dns_san){
        *out_has_dns_san = 0;
    }
    if (!ext_value_der || ext_value_der_len == 0u || !host || host_len == 0u){
        return 0;
    }

    asn1_tlv_t names;
    if (asn1_parse_tlv(ext_value_der, ext_value_der_len, &names) != 0 || names.tag != 0x30u){
        return 0;
    }
    unsigned int off = 0u;
    while (off < names.val_len){
        asn1_tlv_t gn;
        if (asn1_parse_tlv(names.val + off, names.val_len - off, &gn) != 0){
            return 0;
        }
        off += gn.total_len;

        // dNSName [2] IA5String (implicit): tag 0x82
        if (gn.tag == 0x82u){
            if (out_has_dns_san){
                *out_has_dns_san = 1;
            }
            if (hostname_pattern_match(host, host_len, gn.val, gn.val_len)){
                return 1;
            }
        }
    }
    return 0;
}

static int parse_key_usage_bits(const unsigned char* ext_value_der,
                                unsigned int ext_value_der_len,
                                unsigned int* out_bits){
    asn1_tlv_t ku;
    if (!ext_value_der || ext_value_der_len == 0u || !out_bits){
        return -1;
    }
    *out_bits = 0u;
    if (asn1_parse_tlv(ext_value_der, ext_value_der_len, &ku) != 0 || ku.tag != 0x03u || ku.val_len < 1u){
        return -1;
    }
    unsigned int unused = (unsigned int)ku.val[0];
    if (unused > 7u){
        return -1;
    }
    unsigned int bit_count = ((ku.val_len - 1u) * 8u);
    if (bit_count >= unused){
        bit_count -= unused;
    } else{
        bit_count = 0u;
    }
    for (unsigned int i = 0; i < bit_count && i < 16u; i++){
        unsigned int byte_i = 1u + (i / 8u);
        unsigned int bit_i = 7u - (i % 8u);
        if (((ku.val[byte_i] >> bit_i) & 1u) != 0u){
            *out_bits |= (1u << i);
        }
    }
    return 0;
}

static int parse_basic_constraints(const unsigned char* ext_value_der,
                                   unsigned int ext_value_der_len,
                                   int* out_is_ca,
                                   int* out_path_len_present,
                                   unsigned int* out_path_len){
    asn1_tlv_t bc;
    unsigned int off = 0u;
    int is_ca = 0;
    int plen_present = 0;
    unsigned int plen = 0u;
    if (!ext_value_der || ext_value_der_len == 0u ||
        !out_is_ca || !out_path_len_present || !out_path_len){
        return -1;
    }
    if (asn1_parse_tlv(ext_value_der, ext_value_der_len, &bc) != 0 || bc.tag != 0x30u){
        return -1;
    }
    if (off < bc.val_len){
        asn1_tlv_t c;
        if (asn1_parse_tlv(bc.val + off, bc.val_len - off, &c) != 0){
            return -1;
        }
        if (c.tag == 0x01u){
            if (c.val_len != 1u){
                return -1;
            }
            is_ca = (c.val[0] != 0u) ? 1 : 0;
            off += c.total_len;
        }
    }
    if (off < bc.val_len){
        asn1_tlv_t pi;
        const unsigned char* p = 0;
        unsigned int n = 0u;
        if (asn1_parse_tlv(bc.val + off, bc.val_len - off, &pi) != 0 || pi.tag != 0x02u){
            return -1;
        }
        if (asn1_integer_positive_bytes(&pi, &p, &n) != 0 || n > 4u){
            return -1;
        }
        for (unsigned int i = 0; i < n; i++){
            plen = (plen << 8) | p[i];
        }
        plen_present = 1;
        off += pi.total_len;
    }
    if (off != bc.val_len){
        return -1;
    }
    *out_is_ca = is_ca;
    *out_path_len_present = plen_present;
    *out_path_len = plen;
    return 0;
}

static int parse_extended_key_usage(const unsigned char* ext_value_der,
                                    unsigned int ext_value_der_len,
                                    int* out_server_auth,
                                    int* out_any){
    static const unsigned char OID_EKU_SERVER_AUTH[] = { 0x2B,0x06,0x01,0x05,0x05,0x07,0x03,0x01 };
    static const unsigned char OID_EKU_ANY[] = { 0x55,0x1D,0x25,0x00 };
    asn1_tlv_t eku;
    unsigned int off = 0u;
    int saw = 0;
    int saw_any = 0;
    if (!ext_value_der || ext_value_der_len == 0u || !out_server_auth || !out_any){
        return -1;
    }
    if (asn1_parse_tlv(ext_value_der, ext_value_der_len, &eku) != 0 || eku.tag != 0x30u){
        return -1;
    }
    while (off < eku.val_len){
        asn1_tlv_t oid;
        if (asn1_parse_tlv(eku.val + off, eku.val_len - off, &oid) != 0 || oid.tag != 0x06u){
            return -1;
        }
        if (oid_equal(&oid, OID_EKU_SERVER_AUTH, sizeof(OID_EKU_SERVER_AUTH))){
            saw = 1;
        } else if (oid_equal(&oid, OID_EKU_ANY, sizeof(OID_EKU_ANY))){
            saw_any = 1;
        }
        off += oid.total_len;
    }
    *out_server_auth = saw;
    *out_any = saw_any;
    return 0;
}

static int parse_certificate_policies_ext(const unsigned char* ext_value_der,
                                          unsigned int ext_value_der_len,
                                          int* out_has_any){
    static const unsigned char OID_ANY_POLICY[] = { 0x55,0x1D,0x20,0x00 };
    asn1_tlv_t pols;
    unsigned int off = 0u;
    int has_any = 0;
    if (!ext_value_der || ext_value_der_len == 0u || !out_has_any){
        return -1;
    }
    if (asn1_parse_tlv(ext_value_der, ext_value_der_len, &pols) != 0 || pols.tag != 0x30u){
        return -1;
    }
    while (off < pols.val_len){
        asn1_tlv_t pi;
        asn1_tlv_t oid;
        if (asn1_parse_tlv(pols.val + off, pols.val_len - off, &pi) != 0 || pi.tag != 0x30u){
            return -1;
        }
        if (asn1_parse_tlv(pi.val, pi.val_len, &oid) != 0 || oid.tag != 0x06u){
            return -1;
        }
        if (oid_equal(&oid, OID_ANY_POLICY, sizeof(OID_ANY_POLICY))){
            has_any = 1;
        }
        off += pi.total_len;
    }
    *out_has_any = has_any;
    return 0;
}

static int parse_policy_constraints_ext(const unsigned char* ext_value_der,
                                        unsigned int ext_value_der_len,
                                        int* out_req_present,
                                        unsigned int* out_req,
                                        int* out_map_present,
                                        unsigned int* out_map){
    asn1_tlv_t pc;
    unsigned int off = 0u;
    if (!ext_value_der || ext_value_der_len == 0u ||
        !out_req_present || !out_req || !out_map_present || !out_map){
        return -1;
    }
    if (asn1_parse_tlv(ext_value_der, ext_value_der_len, &pc) != 0 || pc.tag != 0x30u){
        return -1;
    }
    while (off < pc.val_len){
        asn1_tlv_t fld;
        if (asn1_parse_tlv(pc.val + off, pc.val_len - off, &fld) != 0){
            return -1;
        }
        if (fld.tag == 0xA0u || fld.tag == 0xA1u){
            asn1_tlv_t iv;
            const unsigned char* p = 0;
            unsigned int n = 0u;
            unsigned int v = 0u;
            if (asn1_parse_tlv(fld.val, fld.val_len, &iv) != 0 || iv.tag != 0x02u){
                return -1;
            }
            if (asn1_integer_positive_bytes(&iv, &p, &n) != 0 || n > 4u){
                return -1;
            }
            for (unsigned int i = 0; i < n; i++){
                v = (v << 8) | p[i];
            }
            if (fld.tag == 0xA0u){
                *out_req_present = 1;
                *out_req = v;
            } else{
                *out_map_present = 1;
                *out_map = v;
            }
        } else{
            return -1;
        }
        off += fld.total_len;
    }
    return 0;
}

static int parse_inhibit_any_policy_ext(const unsigned char* ext_value_der,
                                        unsigned int ext_value_der_len,
                                        unsigned int* out_skip){
    asn1_tlv_t iv;
    const unsigned char* p = 0;
    unsigned int n = 0u;
    unsigned int v = 0u;
    if (!ext_value_der || ext_value_der_len == 0u || !out_skip){
        return -1;
    }
    if (asn1_parse_tlv(ext_value_der, ext_value_der_len, &iv) != 0 || iv.tag != 0x02u){
        return -1;
    }
    if (asn1_integer_positive_bytes(&iv, &p, &n) != 0 || n > 4u){
        return -1;
    }
    for (unsigned int i = 0; i < n; i++){
        v = (v << 8) | p[i];
    }
    *out_skip = v;
    return 0;
}

static int parse_name_constraints_dns_subtrees(const unsigned char* payload,
                                               unsigned int payload_len,
                                               const unsigned char** out_names,
                                               unsigned int* out_lens,
                                               unsigned int* out_count){
    unsigned int off = 0u;
    const unsigned char* seq_bytes = payload;
    unsigned int seq_len = payload_len;
    asn1_tlv_t maybe_seq;
    if (!payload || payload_len == 0u || !out_names || !out_lens || !out_count){
        return -1;
    }

    // Accept either explicit SEQUENCE wrapper or raw SEQUENCE content.
    if (asn1_parse_tlv(payload, payload_len, &maybe_seq) == 0 &&
        maybe_seq.total_len == payload_len && maybe_seq.tag == 0x30u){
        seq_bytes = maybe_seq.val;
        seq_len = maybe_seq.val_len;
    }

    while (off < seq_len){
        asn1_tlv_t subtree;
        asn1_tlv_t base;
        unsigned int soff = 0u;
        if (asn1_parse_tlv(seq_bytes + off, seq_len - off, &subtree) != 0 || subtree.tag != 0x30u){
            return -1;
        }
        off += subtree.total_len;
        if (asn1_parse_tlv(subtree.val + soff, subtree.val_len - soff, &base) != 0){
            return -1;
        }
        soff += base.total_len;

        // RFC5280 profile: minimum must be zero, maximum must be absent.
        while (soff < subtree.val_len){
            asn1_tlv_t opt;
            if (asn1_parse_tlv(subtree.val + soff, subtree.val_len - soff, &opt) != 0){
                return -1;
            }
            if (opt.tag == 0xA0u){
                asn1_tlv_t minv;
                const unsigned char* p = 0;
                unsigned int n = 0u;
                unsigned int minv_int = 0u;
                if (asn1_parse_tlv(opt.val, opt.val_len, &minv) != 0 || minv.tag != 0x02u){
                    return -1;
                }
                if (asn1_integer_positive_bytes(&minv, &p, &n) != 0 || n > 4u){
                    return -1;
                }
                for (unsigned int i = 0; i < n; i++){
                    minv_int = (minv_int << 8) | p[i];
                }
                if (minv_int != 0u){
                    return -1;
                }
            } else if (opt.tag == 0xA1u){
                return -1; // maximum MUST be absent in profile
            }
            soff += opt.total_len;
        }

        // dNSName [2] IA5String
        if (base.tag == 0x82u && base.val_len > 0u){
            if (*out_count >= X509_NC_DNS_MAX){
                return -1;
            }
            out_names[*out_count] = base.val;
            out_lens[*out_count] = base.val_len;
            (*out_count)++;
        }
    }
    return 0;
}

static int parse_name_constraints_ext(const unsigned char* ext_value_der,
                                      unsigned int ext_value_der_len,
                                      parsed_cert_t* out){
    asn1_tlv_t nc;
    unsigned int off = 0u;
    int saw_any = 0;
    if (!ext_value_der || ext_value_der_len == 0u || !out){
        return -1;
    }
    if (asn1_parse_tlv(ext_value_der, ext_value_der_len, &nc) != 0 || nc.tag != 0x30u){
        return -1;
    }
    while (off < nc.val_len){
        asn1_tlv_t fld;
        if (asn1_parse_tlv(nc.val + off, nc.val_len - off, &fld) != 0){
            return -1;
        }
        if (fld.tag == 0xA0u){
            saw_any = 1;
            if (parse_name_constraints_dns_subtrees(fld.val, fld.val_len,
                                                    out->nc_dns_permit,
                                                    out->nc_dns_permit_len,
                                                    &out->nc_dns_permit_count) != 0){
                return -1;
            }
        } else if (fld.tag == 0xA1u){
            saw_any = 1;
            if (parse_name_constraints_dns_subtrees(fld.val, fld.val_len,
                                                    out->nc_dns_exclude,
                                                    out->nc_dns_exclude_len,
                                                    &out->nc_dns_exclude_count) != 0){
                return -1;
            }
        } else{
            return -1;
        }
        off += fld.total_len;
    }
    if (!saw_any){
        return -1;
    }
    return 0;
}

static int parse_cert_tbs(const unsigned char* tbs_tlv,
                          unsigned int tbs_tlv_len,
                          const char* host,
                          unsigned int host_len,
                          int do_hostname_check,
                          parsed_cert_t* out){
    static const unsigned char OID_SAN[] = { 0x55,0x1D,0x11 };
    static const unsigned char OID_BASIC_CONSTRAINTS[] = { 0x55,0x1D,0x13 };
    static const unsigned char OID_KEY_USAGE[] = { 0x55,0x1D,0x0F };
    static const unsigned char OID_CERT_POLICIES[] = { 0x55,0x1D,0x20 };
    static const unsigned char OID_POLICY_MAPPINGS[] = { 0x55,0x1D,0x21 };
    static const unsigned char OID_POLICY_CONSTRAINTS[] = { 0x55,0x1D,0x24 };
    static const unsigned char OID_INHIBIT_ANY_POLICY[] = { 0x55,0x1D,0x36 };
    static const unsigned char OID_NAME_CONSTRAINTS[] = { 0x55,0x1D,0x1E };
    static const unsigned char OID_EKU[] = { 0x55,0x1D,0x25 };
    static const unsigned char OID_SUBJECT_KEY_ID[] = { 0x55,0x1D,0x0E };
    static const unsigned char OID_AUTHORITY_KEY_ID[] = { 0x55,0x1D,0x23 };
    asn1_tlv_t tbs;
    unsigned int off = 0u;
    int has_dns_san = 0;
    int match = 0;

    if (!tbs_tlv || tbs_tlv_len == 0u || !out){
        return -1;
    }
    if (asn1_parse_tlv(tbs_tlv, tbs_tlv_len, &tbs) != 0 || tbs.tag != 0x30u){
        return -1;
    }

    // Optional version [0] EXPLICIT.
    if (off < tbs.val_len && tbs.val[off] == 0xA0u){
        asn1_tlv_t v;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &v) != 0){
            return -1;
        }
        off += v.total_len;
    }

    // serialNumber
    {
        asn1_tlv_t sn;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &sn) != 0 || sn.tag != 0x02u || sn.val_len == 0u){
            return -1;
        }
        out->serial_bytes = sn.val;
        out->serial_len = sn.val_len;
        off += sn.total_len;
    }

    // signature
    {
        asn1_tlv_t sig;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &sig) != 0 || sig.tag != 0x30u){
            return -1;
        }
        off += sig.total_len;
    }

    // issuer Name
    {
        asn1_tlv_t issuer;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &issuer) != 0 || issuer.tag != 0x30u){
            return -1;
        }
        out->issuer_tlv = issuer.hdr;
        out->issuer_tlv_len = issuer.total_len;
        off += issuer.total_len;
    }

    // validity
    {
        asn1_tlv_t validity;
        asn1_tlv_t not_before;
        asn1_tlv_t not_after;
        unsigned int voff = 0u;
        long long nb = 0;
        long long na = 0;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &validity) != 0 || validity.tag != 0x30u){
            return -1;
        }
        if (asn1_parse_tlv(validity.val + voff, validity.val_len - voff, &not_before) != 0){
            return -1;
        }
        voff += not_before.total_len;
        if (asn1_parse_tlv(validity.val + voff, validity.val_len - voff, &not_after) != 0){
            return -1;
        }
        voff += not_after.total_len;
        if (voff != validity.val_len){
            return -1;
        }
        if (parse_asn1_time_to_unix(&not_before, &nb) == 0 &&
            parse_asn1_time_to_unix(&not_after, &na) == 0 &&
            nb <= na){
            out->validity_present = 1;
            out->not_before_unix = nb;
            out->not_after_unix = na;
        }
        off += validity.total_len;
    }

    // subject Name
    {
        asn1_tlv_t subject;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &subject) != 0 || subject.tag != 0x30u){
            return -1;
        }
        out->subject_tlv = subject.hdr;
        out->subject_tlv_len = subject.total_len;
        off += subject.total_len;
    }

    // subjectPublicKeyInfo
    {
        asn1_tlv_t spki;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &spki) != 0 || spki.tag != 0x30u){
            return -1;
        }
        out->spki_tlv = spki.hdr;
        out->spki_tlv_len = spki.total_len;
        off += spki.total_len;
    }

    // Optional fields: issuerUniqueID [1], subjectUniqueID [2], extensions [3]
    while (off < tbs.val_len){
        asn1_tlv_t opt;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &opt) != 0){
            return -1;
        }
        off += opt.total_len;
        if (opt.tag != 0xA3u){
            continue;
        }
        // [3] EXPLICIT Extensions
        asn1_tlv_t exts;
        if (asn1_parse_tlv(opt.val, opt.val_len, &exts) != 0 || exts.tag != 0x30u){
            return -1;
        }

        unsigned int eoff = 0u;
        while (eoff < exts.val_len){
            asn1_tlv_t ext;
            asn1_tlv_t oid;
            asn1_tlv_t ext_value;
            unsigned int xoff = 0u;
            int critical = 0;
            if (asn1_parse_tlv(exts.val + eoff, exts.val_len - eoff, &ext) != 0 || ext.tag != 0x30u){
                return -1;
            }
            eoff += ext.total_len;
            if (asn1_parse_tlv(ext.val + xoff, ext.val_len - xoff, &oid) != 0 || oid.tag != 0x06u){
                return -1;
            }
            xoff += oid.total_len;

            // Optional critical boolean.
            if (xoff < ext.val_len && ext.val[xoff] == 0x01u){
                asn1_tlv_t critical_tlv;
                if (asn1_parse_tlv(ext.val + xoff, ext.val_len - xoff, &critical_tlv) != 0 ||
                    critical_tlv.tag != 0x01u || critical_tlv.val_len != 1u){
                    return -1;
                }
                critical = (critical_tlv.val[0] != 0u) ? 1 : 0;
                xoff += critical_tlv.total_len;
            }
            if (xoff >= ext.val_len){
                return -1;
            }
            if (asn1_parse_tlv(ext.val + xoff, ext.val_len - xoff, &ext_value) != 0 ||
                ext_value.tag != 0x04u){
                return -1;
            }

            if (oid_equal(&oid, OID_SAN, sizeof(OID_SAN))){
                int san_match = parse_san_dns_match(ext_value.val, ext_value.val_len,
                                                    host, host_len, &has_dns_san);
                if (san_match){
                    match = 1;
                }
            } else if (oid_equal(&oid, OID_CERT_POLICIES, sizeof(OID_CERT_POLICIES))){
                int has_any = 0;
                if (parse_certificate_policies_ext(ext_value.val, ext_value.val_len, &has_any) != 0){
                    return -1;
                }
                out->cert_policies_present = 1;
                out->cert_policy_any = has_any;
            } else if (oid_equal(&oid, OID_POLICY_MAPPINGS, sizeof(OID_POLICY_MAPPINGS))){
                out->policy_mappings_present = 1;
            } else if (oid_equal(&oid, OID_POLICY_CONSTRAINTS, sizeof(OID_POLICY_CONSTRAINTS))){
                if (parse_policy_constraints_ext(ext_value.val, ext_value.val_len,
                                                 &out->policy_constraints_req_exp_present,
                                                 &out->policy_constraints_req_exp,
                                                 &out->policy_constraints_inhibit_map_present,
                                                 &out->policy_constraints_inhibit_map) != 0){
                    return -1;
                }
            } else if (oid_equal(&oid, OID_INHIBIT_ANY_POLICY, sizeof(OID_INHIBIT_ANY_POLICY))){
                if (parse_inhibit_any_policy_ext(ext_value.val, ext_value.val_len,
                                                 &out->inhibit_any_policy_skip) != 0){
                    return -1;
                }
                out->inhibit_any_policy_present = 1;
            } else if (oid_equal(&oid, OID_NAME_CONSTRAINTS, sizeof(OID_NAME_CONSTRAINTS))){
                out->name_constraints_present = 1;
                out->name_constraints_critical = critical ? 1 : 0;
                if (parse_name_constraints_ext(ext_value.val, ext_value.val_len, out) != 0){
                    out->name_constraints_parse_error = 1;
                }
            } else if (oid_equal(&oid, OID_BASIC_CONSTRAINTS, sizeof(OID_BASIC_CONSTRAINTS))){
                int is_ca = 0;
                int has_plen = 0;
                unsigned int plen = 0u;
                if (parse_basic_constraints(ext_value.val, ext_value.val_len,
                                            &is_ca, &has_plen, &plen) != 0){
                    return -1;
                }
                out->basic_constraints_present = 1;
                out->is_ca = is_ca;
                out->path_len_present = has_plen;
                out->path_len_constraint = plen;
            } else if (oid_equal(&oid, OID_KEY_USAGE, sizeof(OID_KEY_USAGE))){
                unsigned int ku = 0u;
                if (parse_key_usage_bits(ext_value.val, ext_value.val_len, &ku) != 0){
                    return -1;
                }
                out->key_usage_present = 1;
                out->key_usage_bits = ku;
            } else if (oid_equal(&oid, OID_EKU, sizeof(OID_EKU))){
                int saw_server_auth = 0;
                int saw_any = 0;
                if (parse_extended_key_usage(ext_value.val, ext_value.val_len,
                                             &saw_server_auth, &saw_any) != 0){
                    return -1;
                }
                out->eku_present = 1;
                out->eku_server_auth = saw_server_auth;
                out->eku_any = saw_any;
            } else if (oid_equal(&oid, OID_SUBJECT_KEY_ID, sizeof(OID_SUBJECT_KEY_ID)) ||
                       oid_equal(&oid, OID_AUTHORITY_KEY_ID, sizeof(OID_AUTHORITY_KEY_ID))){
                // Parsed as known extensions for critical-extension policy. We
                // do not currently need their values for chain validation.
            } else if (critical){
                out->unknown_critical_ext = 1;
            }
        }
    }

    if (do_hostname_check){
        if (!has_dns_san){
            match = parse_subject_cn_match(out->subject_tlv, out->subject_tlv_len, host, host_len);
        }
        out->san_present = has_dns_san;
        out->hostname_match = match;
    } else{
        out->san_present = has_dns_san;
        out->hostname_match = 0;
    }
    return 0;
}

static int parse_cert_identity(const unsigned char* der,
                               unsigned int der_len,
                               const char* host,
                               unsigned int host_len,
                               int leaf,
                               parsed_cert_t* out){
    asn1_tlv_t cert;
    unsigned int off = 0u;
    asn1_tlv_t tbs;
    asn1_tlv_t sigalg;
    asn1_tlv_t sigbits;

    if (!der || der_len == 0u || !out){
        return -1;
    }
    out->subject_tlv = 0;
    out->subject_tlv_len = 0;
    out->issuer_tlv = 0;
    out->issuer_tlv_len = 0;
    out->serial_bytes = 0;
    out->serial_len = 0u;
    out->tbs_tlv = 0;
    out->tbs_tlv_len = 0;
    out->spki_tlv = 0;
    out->spki_tlv_len = 0;
    out->sig_bits = 0;
    out->sig_bits_len = 0;
    out->cert_sig_alg = 0u;
    out->key_alg = X509_KEY_ALG_NONE;
    out->rsa_n_len = 0u;
    out->rsa_e_len = 0u;
    out->ec_curve = ECDSA_VERIFY_CURVE_NONE;
    out->ec_q_len = 0u;
    out->basic_constraints_present = 0;
    out->is_ca = 0;
    out->path_len_present = 0;
    out->path_len_constraint = 0u;
    out->key_usage_present = 0;
    out->key_usage_bits = 0u;
    out->cert_policies_present = 0;
    out->cert_policy_any = 0;
    out->policy_mappings_present = 0;
    out->policy_constraints_req_exp_present = 0;
    out->policy_constraints_req_exp = 0u;
    out->policy_constraints_inhibit_map_present = 0;
    out->policy_constraints_inhibit_map = 0u;
    out->inhibit_any_policy_present = 0;
    out->inhibit_any_policy_skip = 0u;
    out->name_constraints_present = 0;
    out->name_constraints_critical = 0;
    out->name_constraints_parse_error = 0;
    out->nc_dns_permit_count = 0u;
    out->nc_dns_exclude_count = 0u;
    for (unsigned int i = 0; i < X509_NC_DNS_MAX; i++){
        out->nc_dns_permit[i] = 0;
        out->nc_dns_permit_len[i] = 0u;
        out->nc_dns_exclude[i] = 0;
        out->nc_dns_exclude_len[i] = 0u;
    }
    out->eku_present = 0;
    out->eku_server_auth = 0;
    out->eku_any = 0;
    out->unknown_critical_ext = 0;
    out->validity_present = 0;
    out->not_before_unix = 0;
    out->not_after_unix = 0;
    out->san_present = 0;
    out->hostname_match = 0;

    if (asn1_parse_tlv(der, der_len, &cert) != 0 || cert.tag != 0x30u || cert.total_len != der_len){
        return -1;
    }

    if (asn1_parse_tlv(cert.val + off, cert.val_len - off, &tbs) != 0 || tbs.tag != 0x30u){
        return -1;
    }
    out->tbs_tlv = tbs.hdr;
    out->tbs_tlv_len = tbs.total_len;
    off += tbs.total_len;

    if (asn1_parse_tlv(cert.val + off, cert.val_len - off, &sigalg) != 0 || sigalg.tag != 0x30u){
        return -1;
    }
    off += sigalg.total_len;

    if (asn1_parse_tlv(cert.val + off, cert.val_len - off, &sigbits) != 0 ||
        sigbits.tag != 0x03u || sigbits.val_len < 2u || sigbits.val[0] != 0u){
        return -1;
    }
    out->sig_bits = sigbits.val + 1u;
    out->sig_bits_len = sigbits.val_len - 1u;
    off += sigbits.total_len;
    if (off != cert.val_len){
        return -1;
    }

    (void)parse_signature_algorithm(&sigalg, &out->cert_sig_alg);

    if (parse_cert_tbs(tbs.hdr, tbs.total_len,
                       host, host_len,
                       leaf ? 1 : 0, out) != 0){
        return -1;
    }

    if (parse_rsa_spki(out->spki_tlv, out->spki_tlv_len,
                       out->rsa_n, &out->rsa_n_len,
                       out->rsa_e, &out->rsa_e_len) == 0){
        out->key_alg = X509_KEY_ALG_RSA;
    } else if (parse_ec_spki(out->spki_tlv, out->spki_tlv_len,
                             &out->ec_curve,
                             out->ec_qx, out->ec_qy,
                             &out->ec_q_len) == 0){
        out->key_alg = X509_KEY_ALG_EC;
    }

    return 0;
}

static int name_hash(const unsigned char* tlv, unsigned int tlv_len, unsigned char out[32]){
    if (!tlv || tlv_len == 0u || !out){
        return -1;
    }
    sha256_digest(tlv, tlv_len, out);
    return 0;
}

static int cert_der_hash(const unsigned char* der, unsigned int der_len, unsigned char out[32]){
    if (!der || der_len == 0u || !out){
        return -1;
    }
    sha256_digest(der, der_len, out);
    return 0;
}

static int serial_hash(const unsigned char* serial, unsigned int serial_len, unsigned char out[32]){
    if (!serial || serial_len == 0u || !out){
        return -1;
    }
    sha256_digest(serial, serial_len, out);
    return 0;
}

static int base64_val(unsigned char c){
    if (c >= 'A' && c <= 'Z'){
        return (int)(c - 'A');
    }
    if (c >= 'a' && c <= 'z'){
        return (int)(c - 'a') + 26;
    }
    if (c >= '0' && c <= '9'){
        return (int)(c - '0') + 52;
    }
    if (c == '+'){
        return 62;
    }
    if (c == '/'){
        return 63;
    }
    if (c == '='){
        return -2;
    }
    return -1;
}

static int decode_pem_block_to_der(const unsigned char* b64,
                                   unsigned int b64_len,
                                   unsigned char* out_der,
                                   unsigned int out_cap,
                                   unsigned int* out_len){
    unsigned int o = 0u;
    unsigned int i = 0u;
    int q[4];
    unsigned int qn = 0u;

    if (!b64 || !out_der || !out_len){
        return -1;
    }
    *out_len = 0u;

    while (i < b64_len){
        int v = base64_val(b64[i++]);
        if (v == -1){
            continue; // whitespace/comments
        }
        q[qn++] = v;
        if (qn < 4u){
            continue;
        }

        if (q[0] < 0 || q[1] < 0){
            return -1;
        }
        unsigned int triple = ((unsigned int)q[0] << 18) |
                              ((unsigned int)q[1] << 12) |
                              ((unsigned int)((q[2] < 0) ? 0 : q[2]) << 6) |
                              (unsigned int)((q[3] < 0) ? 0 : q[3]);

        if (o >= out_cap){
            return -1;
        }
        out_der[o++] = (unsigned char)((triple >> 16) & 0xFFu);

        if (q[2] != -2){
            if (o >= out_cap){
                return -1;
            }
            out_der[o++] = (unsigned char)((triple >> 8) & 0xFFu);
        }
        if (q[3] != -2){
            if (o >= out_cap){
                return -1;
            }
            out_der[o++] = (unsigned char)(triple & 0xFFu);
        }

        if (q[2] == -2 || q[3] == -2){
            break;
        }
        qn = 0u;
    }

    *out_len = o;
    return (o > 0u) ? 0 : -1;
}

static int find_next_marker(const unsigned char* buf,
                            unsigned int len,
                            unsigned int start,
                            const char* marker,
                            unsigned int marker_len){
    if (!buf || !marker || marker_len == 0u || start >= len){
        return -1;
    }
    for (unsigned int i = start; (i + marker_len) <= len; i++){
        unsigned int j = 0u;
        while (j < marker_len && buf[i + j] == (unsigned char)marker[j]){
            j++;
        }
        if (j == marker_len){
            return (int)i;
        }
    }
    return -1;
}

static int parse_ca_bundle_anchors(const unsigned char* pem,
                                   unsigned int pem_len){
    static const char BEGIN_MARK[] = "-----BEGIN CERTIFICATE-----";
    static const char END_MARK[]   = "-----END CERTIFICATE-----";
    unsigned int pos = 0u;
    unsigned int anchors = 0u;

    if (!pem || pem_len == 0u){
        return -1;
    }

    while (anchors < X509_MAX_CA_ANCHORS){
        int b = find_next_marker(pem, pem_len, pos, BEGIN_MARK, (unsigned int)(sizeof(BEGIN_MARK) - 1u));
        if (b < 0){
            break;
        }
        unsigned int after_begin = (unsigned int)b + (unsigned int)(sizeof(BEGIN_MARK) - 1u);
        int e = find_next_marker(pem, pem_len, after_begin, END_MARK, (unsigned int)(sizeof(END_MARK) - 1u));
        if (e < 0){
            break;
        }
        unsigned int b64_start = after_begin;
        unsigned int b64_len = (unsigned int)e - b64_start;
        unsigned int der_len = 0u;
        if (decode_pem_block_to_der(&pem[b64_start], b64_len,
                                    g_ca_der_tmp, sizeof(g_ca_der_tmp),
                                    &der_len) == 0 && der_len > 0u){
            parsed_cert_t ci;
            if (parse_cert_identity(g_ca_der_tmp, der_len, "x", 1u, 0, &ci) == 0 &&
                ci.subject_tlv && ci.subject_tlv_len > 0u){
                if (name_hash(ci.subject_tlv, ci.subject_tlv_len, g_ca_anchors[anchors].subject_hash) == 0 &&
                    cert_der_hash(g_ca_der_tmp, der_len, g_ca_anchors[anchors].der_hash) == 0){
                    g_ca_anchors[anchors].key_alg = ci.key_alg;
                    g_ca_anchors[anchors].rsa_n_len = ci.rsa_n_len;
                    g_ca_anchors[anchors].rsa_e_len = ci.rsa_e_len;
                    g_ca_anchors[anchors].ec_curve = ci.ec_curve;
                    g_ca_anchors[anchors].ec_q_len = ci.ec_q_len;
                    g_ca_anchors[anchors].basic_constraints_present = ci.basic_constraints_present;
                    g_ca_anchors[anchors].is_ca = ci.is_ca;
                    g_ca_anchors[anchors].path_len_present = ci.path_len_present;
                    g_ca_anchors[anchors].path_len_constraint = ci.path_len_constraint;
                    g_ca_anchors[anchors].key_usage_present = ci.key_usage_present;
                    g_ca_anchors[anchors].key_usage_bits = ci.key_usage_bits;
                    for (unsigned int k = 0; k < ci.rsa_n_len; k++){
                        g_ca_anchors[anchors].rsa_n[k] = ci.rsa_n[k];
                    }
                    for (unsigned int k = 0; k < ci.rsa_e_len; k++){
                        g_ca_anchors[anchors].rsa_e[k] = ci.rsa_e[k];
                    }
                    for (unsigned int k = 0; k < ci.ec_q_len; k++){
                        g_ca_anchors[anchors].ec_qx[k] = ci.ec_qx[k];
                        g_ca_anchors[anchors].ec_qy[k] = ci.ec_qy[k];
                    }
                    anchors++;
                }
            }
        }
        pos = (unsigned int)e + (unsigned int)(sizeof(END_MARK) - 1u);
    }

    if (anchors == 0u){
        return -1;
    }
    g_ca_anchor_count = anchors;
    return 0;
}

static int verify_ca_bundle_ed25519(const unsigned char* pem_bytes,
                                    unsigned int pem_len){
    const trust_key_t* key = trust_find_key(TRUST_KEY_ID_ADMIN_MAIN);
    unsigned char digest[32];
    unsigned char msg[16u + 4u + 4u + 4u + 8u + 1u + X509_CA_LABEL_LEN + 32u];
    unsigned int o = 0u;
    static const unsigned char tag[16] = {
        'Q','O','S','-','F','I','L','E','-','S','I','G','-','V','1','\0'
    };
    int sig_n;

    if (!key || key->revoked){
        return -1;
    }

    sig_n = fat32_read_file(X509_CA_SIG_FAT, g_ca_sig_buf, (int)sizeof(g_ca_sig_buf));
    if (sig_n != 64){
        uart_puts("X509: CA_ROOTS.SIG read failed or wrong size.\n");
        return -1;
    }

    sha256_digest(pem_bytes, pem_len, digest);
    for (unsigned int i = 0; i < 16u; i++) msg[o++] = tag[i];
    put_u32_le(msg + o, 1u); o += 4u; // schema version
    put_u32_le(msg + o, TRUST_KEY_ID_ADMIN_MAIN); o += 4u;
    put_u32_le(msg + o, QOS_SIG_ALG_ED25519); o += 4u;
    put_u64_le(msg + o, (unsigned long long)pem_len); o += 8u;
    msg[o++] = (unsigned char)X509_CA_LABEL_LEN;
    for (unsigned int i = 0; i < X509_CA_LABEL_LEN; i++) msg[o++] = (unsigned char)X509_CA_LABEL[i];
    for (unsigned int i = 0; i < 32u; i++) msg[o++] = digest[i];

    if (!qos_ed25519_verify(g_ca_sig_buf, msg, o, key->ed25519_pubkey)){
        return -1;
    }
    return 0;
}

static int verify_ca_bundle_pq_optional(const unsigned char* pem_bytes,
                                        unsigned int pem_len){
    const trust_key_t* key = trust_find_key(TRUST_KEY_ID_ADMIN_MAIN);
    int n;
    unsigned int magic;
    unsigned int version;
    unsigned int signer_key_id;
    unsigned int sig_alg;
    unsigned int sig_len;
    unsigned char digest[32];
    unsigned char msg[16u + 4u + 4u + 4u + 8u + 1u + X509_CA_LABEL_LEN + 32u];
    unsigned int o = 0u;
    static const unsigned char tag[16] = {
        'Q','O','S','-','F','I','L','E','-','S','I','G','-','V','1','\0'
    };

    if (!key || key->revoked){
        return -1;
    }

    n = fat32_read_file(X509_CA_PQS_FAT, g_ca_pqs_buf, (int)sizeof(g_ca_pqs_buf));
    if (n <= 0){
        uart_puts("X509: CA_ROOTS.PQS not present; using Ed25519 CA bundle signature.\n");
        return 0; // optional
    }
    if (n < (int)QOS_PQ_SIG_HEADER_BYTES){
        return -1;
    }

    magic = get_u32_le(&g_ca_pqs_buf[0]);
    version = get_u32_le(&g_ca_pqs_buf[4]);
    signer_key_id = get_u32_le(&g_ca_pqs_buf[8]);
    sig_alg = get_u32_le(&g_ca_pqs_buf[12]);
    sig_len = get_u32_le(&g_ca_pqs_buf[16]);
    if (magic != QOS_PQ_SIG_MAGIC || version != QOS_PQ_SIG_VERSION){
        return -1;
    }
    if (signer_key_id != TRUST_KEY_ID_ADMIN_MAIN){
        return -1;
    }
    if ((QOS_PQ_SIG_HEADER_BYTES + sig_len) > (unsigned int)n){
        return -1;
    }
    if (key->pq_pubkey_len == 0u){
        return -1;
    }

    sha256_digest(pem_bytes, pem_len, digest);
    for (unsigned int i = 0; i < 16u; i++) msg[o++] = tag[i];
    put_u32_le(msg + o, 1u); o += 4u;
    put_u32_le(msg + o, TRUST_KEY_ID_ADMIN_MAIN); o += 4u;
    put_u32_le(msg + o, QOS_SIG_ALG_ED25519); o += 4u;
    put_u64_le(msg + o, (unsigned long long)pem_len); o += 8u;
    msg[o++] = (unsigned char)X509_CA_LABEL_LEN;
    for (unsigned int i = 0; i < X509_CA_LABEL_LEN; i++) msg[o++] = (unsigned char)X509_CA_LABEL[i];
    for (unsigned int i = 0; i < 32u; i++) msg[o++] = digest[i];

    sha256_digest(msg, o, digest);
    if (pq_sig_verify_digest_sha256(sig_alg,
                                    digest,
                                    &g_ca_pqs_buf[QOS_PQ_SIG_HEADER_BYTES],
                                    sig_len,
                                    key->pq_pubkey, key->pq_pubkey_len) != 0){
        return -1;
    }
    return 0;
}

static int verify_revocation_ed25519(const unsigned char* rev_bytes,
                                     unsigned int rev_len){
    const trust_key_t* key = trust_find_key(TRUST_KEY_ID_ADMIN_MAIN);
    unsigned char digest[32];
    unsigned char msg[16u + 4u + 4u + 4u + 8u + 1u + X509_REVOKE_LABEL_LEN + 32u];
    unsigned int o = 0u;
    static const unsigned char tag[16] = {
        'Q','O','S','-','F','I','L','E','-','S','I','G','-','V','1','\0'
    };
    int sig_n;

    if (!key || key->revoked){
        return -1;
    }

    sig_n = fat32_read_file(X509_REVOKE_SIG_FAT, g_revoke_sig_buf, (int)sizeof(g_revoke_sig_buf));
    if (sig_n != 64){
        uart_puts("X509: revocation .SIG missing or wrong size.\n");
        return -1;
    }

    sha256_digest(rev_bytes, rev_len, digest);
    for (unsigned int i = 0; i < 16u; i++) msg[o++] = tag[i];
    put_u32_le(msg + o, 1u); o += 4u; // schema version
    put_u32_le(msg + o, TRUST_KEY_ID_ADMIN_MAIN); o += 4u;
    put_u32_le(msg + o, QOS_SIG_ALG_ED25519); o += 4u;
    put_u64_le(msg + o, (unsigned long long)rev_len); o += 8u;
    msg[o++] = (unsigned char)X509_REVOKE_LABEL_LEN;
    for (unsigned int i = 0; i < X509_REVOKE_LABEL_LEN; i++) msg[o++] = (unsigned char)X509_REVOKE_LABEL[i];
    for (unsigned int i = 0; i < 32u; i++) msg[o++] = digest[i];

    if (!qos_ed25519_verify(g_revoke_sig_buf, msg, o, key->ed25519_pubkey)){
        return -1;
    }
    return 0;
}

static int verify_revocation_pq_optional(const unsigned char* rev_bytes,
                                         unsigned int rev_len){
    const trust_key_t* key = trust_find_key(TRUST_KEY_ID_ADMIN_MAIN);
    int n;
    unsigned int magic;
    unsigned int version;
    unsigned int signer_key_id;
    unsigned int sig_alg;
    unsigned int sig_len;
    unsigned char digest[32];
    unsigned char msg[16u + 4u + 4u + 4u + 8u + 1u + X509_REVOKE_LABEL_LEN + 32u];
    unsigned int o = 0u;
    static const unsigned char tag[16] = {
        'Q','O','S','-','F','I','L','E','-','S','I','G','-','V','1','\0'
    };

    if (!key || key->revoked){
        return -1;
    }

    n = fat32_read_file(X509_REVOKE_PQS_FAT, g_revoke_pqs_buf, (int)sizeof(g_revoke_pqs_buf));
    if (n <= 0){
        return 0; // optional
    }
    if (n < (int)QOS_PQ_SIG_HEADER_BYTES){
        return -1;
    }

    magic = get_u32_le(&g_revoke_pqs_buf[0]);
    version = get_u32_le(&g_revoke_pqs_buf[4]);
    signer_key_id = get_u32_le(&g_revoke_pqs_buf[8]);
    sig_alg = get_u32_le(&g_revoke_pqs_buf[12]);
    sig_len = get_u32_le(&g_revoke_pqs_buf[16]);
    if (magic != QOS_PQ_SIG_MAGIC || version != QOS_PQ_SIG_VERSION){
        return -1;
    }
    if (signer_key_id != TRUST_KEY_ID_ADMIN_MAIN){
        return -1;
    }
    if ((QOS_PQ_SIG_HEADER_BYTES + sig_len) > (unsigned int)n){
        return -1;
    }
    if (key->pq_pubkey_len == 0u){
        return -1;
    }

    sha256_digest(rev_bytes, rev_len, digest);
    for (unsigned int i = 0; i < 16u; i++) msg[o++] = tag[i];
    put_u32_le(msg + o, 1u); o += 4u;
    put_u32_le(msg + o, TRUST_KEY_ID_ADMIN_MAIN); o += 4u;
    put_u32_le(msg + o, QOS_SIG_ALG_ED25519); o += 4u;
    put_u64_le(msg + o, (unsigned long long)rev_len); o += 8u;
    msg[o++] = (unsigned char)X509_REVOKE_LABEL_LEN;
    for (unsigned int i = 0; i < X509_REVOKE_LABEL_LEN; i++) msg[o++] = (unsigned char)X509_REVOKE_LABEL[i];
    for (unsigned int i = 0; i < 32u; i++) msg[o++] = digest[i];

    sha256_digest(msg, o, digest);
    if (pq_sig_verify_digest_sha256(sig_alg,
                                    digest,
                                    &g_revoke_pqs_buf[QOS_PQ_SIG_HEADER_BYTES],
                                    sig_len,
                                    key->pq_pubkey, key->pq_pubkey_len) != 0){
        return -1;
    }
    return 0;
}

static int parse_revocation_table(const unsigned char* rev_bytes,
                                  unsigned int rev_len){
    // Format:
    // u32 magic "QREV" (little-endian)
    // u32 version (1)
    // u32 entry_count
    // u32 reserved
    // entries[entry_count]: { issuer_name_hash[32], serial_sha256[32] }
    const unsigned int hdr_len = 16u;
    unsigned int magic;
    unsigned int version;
    unsigned int count;
    unsigned int need;
    if (!rev_bytes || rev_len < hdr_len){
        return -1;
    }
    magic = get_u32_le(&rev_bytes[0]);
    version = get_u32_le(&rev_bytes[4]);
    count = get_u32_le(&rev_bytes[8]);
    if (magic != ((unsigned int)'Q' | ((unsigned int)'R' << 8) | ((unsigned int)'E' << 16) | ((unsigned int)'V' << 24))){
        return -1;
    }
    if (version != 1u || count > X509_REVOKE_MAX_ENTRIES){
        return -1;
    }
    need = hdr_len + (count * 64u);
    if (need > rev_len){
        return -1;
    }
    g_revoked_count = 0u;
    for (unsigned int i = 0; i < count; i++){
        unsigned int base = hdr_len + (i * 64u);
        for (unsigned int j = 0; j < 32u; j++){
            g_revoked[i].issuer_hash[j] = rev_bytes[base + j];
            g_revoked[i].serial_hash[j] = rev_bytes[base + 32u + j];
        }
        g_revoked_count++;
    }
    return 0;
}

static int ensure_revocation_loaded(void){
    int n;
    if (g_revoke_cache_state == 1){
        return 0;
    }
    if (g_revoke_cache_state == 2){
        return g_require_revocation_list ? -1 : 0;
    }
    if (g_revoke_cache_state < 0){
        return -1;
    }

    g_revoked_count = 0u;
    n = fat32_read_file(X509_REVOKE_BIN_FAT, g_revoke_bin_buf, (int)sizeof(g_revoke_bin_buf));
    if (n <= 0){
        g_revoke_cache_state = 2;
        if (g_require_revocation_list){
            uart_puts("X509: revocation list required but missing.\n");
            return -1;
        }
        return 0;
    }
    if (n >= (int)sizeof(g_revoke_bin_buf)){
        g_revoke_cache_state = -1;
        return -1;
    }
    if (verify_revocation_ed25519(g_revoke_bin_buf, (unsigned int)n) != 0){
        uart_puts("X509: revocation .SIG verify failed.\n");
        g_revoke_cache_state = -1;
        return -1;
    }
    if (verify_revocation_pq_optional(g_revoke_bin_buf, (unsigned int)n) != 0){
        uart_puts("X509: revocation .PQS verify failed.\n");
        g_revoke_cache_state = -1;
        return -1;
    }
    if (parse_revocation_table(g_revoke_bin_buf, (unsigned int)n) != 0){
        uart_puts("X509: revocation table parse failed.\n");
        g_revoke_cache_state = -1;
        return -1;
    }
    g_revoke_cache_state = 1;
    uart_puts("X509: revocation entries=");
    uart_putdec((unsigned long)g_revoked_count);
    uart_puts("\n");
    return 0;
}

static int ensure_ca_anchors_loaded(void){
    int n;
    if (g_ca_cache_state == 1){
        return 0;
    }
    if (g_ca_cache_state < 0){
        return -1;
    }

    g_ca_anchor_count = 0u;

    if (fat32_init() != 0){
        g_ca_cache_state = -1;
        return -1;
    }
    n = fat32_read_file(X509_CA_PEM_FAT, g_ca_pem_buf, X509_CA_PEM_MAX);
    if (n <= 0){
        uart_puts("X509: CA_ROOTS.PEM read failed.\n");
        g_ca_cache_state = -1;
        return -1;
    }
    if (n >= (int)X509_CA_PEM_MAX){
        uart_puts("X509: CA_ROOTS.PEM exceeds verifier buffer.\n");
        g_ca_cache_state = -1;
        return -1;
    }
    g_ca_pem_buf[n] = 0;

    if (verify_ca_bundle_ed25519(g_ca_pem_buf, (unsigned int)n) != 0){
        uart_puts("X509: CA bundle Ed25519 signature verify failed.\n");
        g_ca_cache_state = -1;
        return -1;
    }
    if (verify_ca_bundle_pq_optional(g_ca_pem_buf, (unsigned int)n) != 0){
        uart_puts("X509: CA bundle PQ signature verify failed.\n");
        g_ca_cache_state = -1;
        return -1;
    }
    if (parse_ca_bundle_anchors(g_ca_pem_buf, (unsigned int)n) != 0){
        uart_puts("X509: CA_ROOTS.PEM parsed no anchors.\n");
        g_ca_cache_state = -1;
        return -1;
    }
    uart_puts("X509: CA roots loaded anchors=");
    uart_putdec((unsigned long)g_ca_anchor_count);
    uart_puts("\n");
    g_ca_cache_state = 1;
    return 0;
}

static int anchor_contains_der_hash(const unsigned char der_hash[32]){
    for (unsigned int i = 0; i < g_ca_anchor_count; i++){
        if (crypto_consttime_equal(g_ca_anchors[i].der_hash, der_hash, 32u)){
            return 1;
        }
    }
    return 0;
}

static int anchor_find_subject_hash(const unsigned char subject_hash[32]){
    for (unsigned int i = 0; i < g_ca_anchor_count; i++){
        if (crypto_consttime_equal(g_ca_anchors[i].subject_hash, subject_hash, 32u)){
            return (int)i;
        }
    }
    return -1;
}

static int cert_is_revoked(const unsigned char issuer_hash[32],
                           const parsed_cert_t* cert){
    unsigned char sh[32];
    if (!issuer_hash || !cert || !cert->serial_bytes || cert->serial_len == 0u){
        return 0;
    }
    if (serial_hash(cert->serial_bytes, cert->serial_len, sh) != 0){
        return 0;
    }
    for (unsigned int i = 0; i < g_revoked_count; i++){
        if (crypto_consttime_equal(issuer_hash, g_revoked[i].issuer_hash, 32u) &&
            crypto_consttime_equal(sh, g_revoked[i].serial_hash, 32u)){
            return 1;
        }
    }
    return 0;
}

static int cert_is_self_issued(const parsed_cert_t* cert){
    if (!cert || !cert->subject_tlv || !cert->issuer_tlv){
        return 0;
    }
    if (cert->subject_tlv_len != cert->issuer_tlv_len){
        return 0;
    }
    return memcmp(cert->subject_tlv, cert->issuer_tlv, cert->subject_tlv_len) == 0;
}

static int leaf_host_allowed_by_name_constraints(const parsed_cert_t* ca,
                                                 const char* host,
                                                 unsigned int host_len){
    int permit_ok = 0;
    if (!ca || !host || host_len == 0u){
        return -1;
    }
    if (!ca->name_constraints_present){
        return 0;
    }
    if (ca->name_constraints_parse_error){
        return -1;
    }

    // Excluded always wins.
    for (unsigned int i = 0; i < ca->nc_dns_exclude_count; i++){
        if (dns_subtree_match(host, host_len, ca->nc_dns_exclude[i], ca->nc_dns_exclude_len[i])){
            return -1;
        }
    }

    // If permitted set exists, leaf must match at least one.
    if (ca->nc_dns_permit_count > 0u){
        for (unsigned int i = 0; i < ca->nc_dns_permit_count; i++){
            if (dns_subtree_match(host, host_len, ca->nc_dns_permit[i], ca->nc_dns_permit_len[i])){
                permit_ok = 1;
                break;
            }
        }
        if (!permit_ok){
            return -1;
        }
    }
    return 0;
}

static int cert_time_valid_now(const parsed_cert_t* cert, long long now_unix){
    if (!cert || !cert->validity_present){
        return -1;
    }
    if (now_unix < cert->not_before_unix || now_unix > cert->not_after_unix){
        return -1;
    }
    return 0;
}

static int cert_is_ca_usable(const parsed_cert_t* cert){
    if (!cert){
        return 0;
    }
    if (!cert->basic_constraints_present || !cert->is_ca){
        return 0;
    }
    if (cert->key_usage_present &&
        (cert->key_usage_bits & X509_KU_KEY_CERT_SIGN) == 0u){
        return 0;
    }
    if (cert->eku_present && !(cert->eku_server_auth || cert->eku_any)){
        return 0;
    }
    return 1;
}

static int anchor_is_ca_usable(const ca_anchor_t* anchor){
    if (!anchor){
        return 0;
    }
    if (anchor->basic_constraints_present && !anchor->is_ca){
        return 0;
    }
    if (anchor->key_usage_present &&
        (anchor->key_usage_bits & X509_KU_KEY_CERT_SIGN) == 0u){
        return 0;
    }
    return 1;
}

static int verify_cert_with_rsa_key(const parsed_cert_t* child,
                                    const unsigned char* rsa_n,
                                    unsigned int rsa_n_len,
                                    const unsigned char* rsa_e,
                                    unsigned int rsa_e_len){
    if (!child || !rsa_n || !rsa_e ||
        !child->tbs_tlv || child->tbs_tlv_len == 0u ||
        !child->sig_bits || child->sig_bits_len == 0u ||
        child->cert_sig_alg == 0u){
        return -1;
    }
    return rsa_verify_x509_signature(rsa_n, rsa_n_len,
                                     rsa_e, rsa_e_len,
                                     child->cert_sig_alg,
                                     child->tbs_tlv,
                                     child->tbs_tlv_len,
                                     child->sig_bits,
                                     child->sig_bits_len);
}

static int verify_cert_with_ec_key(const parsed_cert_t* child,
                                   unsigned int ec_curve,
                                   const unsigned char* ec_qx,
                                   const unsigned char* ec_qy,
                                   unsigned int ec_q_len){
    if (!child || !ec_qx || !ec_qy ||
        !child->tbs_tlv || child->tbs_tlv_len == 0u ||
        !child->sig_bits || child->sig_bits_len == 0u ||
        child->cert_sig_alg == 0u){
        return -1;
    }
    return ecdsa_verify_signature(ec_curve,
                                  ec_qx, ec_qy, ec_q_len,
                                  child->cert_sig_alg,
                                  child->tbs_tlv, child->tbs_tlv_len,
                                  child->sig_bits, child->sig_bits_len);
}

static int verify_cert_signed_by_cert(const parsed_cert_t* child,
                                      const parsed_cert_t* issuer){
    if (!child || !issuer){
        return -1;
    }
    if (issuer->key_alg == X509_KEY_ALG_RSA){
        return verify_cert_with_rsa_key(child,
                                        issuer->rsa_n, issuer->rsa_n_len,
                                        issuer->rsa_e, issuer->rsa_e_len);
    }
    if (issuer->key_alg == X509_KEY_ALG_EC){
        return verify_cert_with_ec_key(child,
                                       issuer->ec_curve,
                                       issuer->ec_qx, issuer->ec_qy,
                                       issuer->ec_q_len);
    }
    return -1;
}

static int verify_cert_signed_by_anchor(const parsed_cert_t* child,
                                        const ca_anchor_t* anchor){
    if (!child || !anchor){
        return -1;
    }
    if (anchor->key_alg == X509_KEY_ALG_RSA){
        return verify_cert_with_rsa_key(child,
                                        anchor->rsa_n, anchor->rsa_n_len,
                                        anchor->rsa_e, anchor->rsa_e_len);
    }
    if (anchor->key_alg == X509_KEY_ALG_EC){
        return verify_cert_with_ec_key(child,
                                       anchor->ec_curve,
                                       anchor->ec_qx, anchor->ec_qy,
                                       anchor->ec_q_len);
    }
    return -1;
}

void x509_verify_reset_cache(void){
    g_ca_anchor_count = 0u;
    g_ca_cache_state = 0;
    g_revoked_count = 0u;
    g_revoke_cache_state = 0;
    crypto_memzero(g_ca_pem_buf, sizeof(g_ca_pem_buf));
    crypto_memzero(g_ca_sig_buf, sizeof(g_ca_sig_buf));
    crypto_memzero(g_ca_pqs_buf, sizeof(g_ca_pqs_buf));
    crypto_memzero(g_ca_anchors, sizeof(g_ca_anchors));
    crypto_memzero(g_revoke_bin_buf, sizeof(g_revoke_bin_buf));
    crypto_memzero(g_revoke_sig_buf, sizeof(g_revoke_sig_buf));
    crypto_memzero(g_revoke_pqs_buf, sizeof(g_revoke_pqs_buf));
    crypto_memzero(g_revoked, sizeof(g_revoked));
}

void x509_set_validation_time_unix(long long unix_time){
    if (unix_time <= 0){
        return;
    }
    g_validation_time_unix = unix_time;
    g_validation_time_set = 1;
    g_validation_time_source = 2;
}

void x509_clear_validation_time(void){
    g_validation_time_unix = 0;
    g_validation_time_set = 0;
    g_validation_time_source = 0;
}

void x509_require_explicit_validation_time(int required){
    g_require_explicit_validation_time = required ? 1 : 0;
}

void x509_require_revocation_list(int required){
    g_require_revocation_list = required ? 1 : 0;
}

int x509_verify_tls13_certificate(const char* host,
                                  const unsigned char* cert_body,
                                  unsigned int cert_body_len,
                                  unsigned short server_cert_verify_alg,
                                  x509_verify_result_t* out_result){
    const unsigned char* cert_der[X509_MAX_CHAIN_CERTS];
    unsigned int cert_der_len[X509_MAX_CHAIN_CERTS];
    parsed_cert_t certs[X509_MAX_CHAIN_CERTS];
    unsigned int cert_count = 0u;
    unsigned int off = 0u;
    unsigned int ctx_len;
    unsigned int list_len;
    unsigned int list_end;
    unsigned int host_len;
    unsigned char subject_hashes[X509_MAX_CHAIN_CERTS][32];
    unsigned char issuer_hashes[X509_MAX_CHAIN_CERTS][32];
    unsigned char cur_der_hash[32];
    int used[X509_MAX_CHAIN_CERTS];
    unsigned int path_idx[X509_MAX_CHAIN_CERTS];
    unsigned int path_len = 0u;
    int anchor_ok = 0;
    int anchor_idx = -1;
    int anchor_in_chain = 0;
    long long now_unix = 0;
    unsigned int explicit_policy = 0u;
    unsigned int inhibit_any_policy = 0u;
    unsigned int policy_mapping = 0u;

    if (out_result){
        out_result->chain_certs = 0u;
        out_result->anchor_count = g_ca_anchor_count;
        out_result->leaf_cert_sig_alg = 0u;
        out_result->leaf_key_alg = X509_VERIFY_KEY_NONE;
        out_result->leaf_rsa_n_len = 0u;
        out_result->leaf_rsa_e_len = 0u;
        out_result->leaf_ec_curve = ECDSA_VERIFY_CURVE_NONE;
        out_result->leaf_ec_q_len = 0u;
        out_result->hostname_ok = 0;
        out_result->chain_anchor_ok = 0;
    }

    if (!host || !*host || !cert_body || cert_body_len < 4u){
        return -1;
    }
    host_len = (unsigned int)trim_trailing_dot_len(host, (unsigned int)kstrlen(host));
    if (host_len == 0u){
        return -1;
    }

    seed_validation_time_from_build_if_unset();
    if (!g_validation_time_set){
        uart_puts("X509: validation time not set.\n");
        return -1;
    }
    if (g_require_explicit_validation_time && g_validation_time_source != 2){
        uart_puts("X509: explicit validation time required.\n");
        return -1;
    }
    now_unix = g_validation_time_unix;

    if (ensure_ca_anchors_loaded() != 0){
        return -1;
    }
    if (ensure_revocation_loaded() != 0){
        return -1;
    }

    ctx_len = cert_body[off++];
    if ((off + ctx_len) > cert_body_len){
        return -1;
    }
    off += ctx_len;
    if ((off + 3u) > cert_body_len){
        return -1;
    }
    list_len = be24_read(&cert_body[off]);
    off += 3u;
    if ((off + list_len) > cert_body_len){
        return -1;
    }
    list_end = off + list_len;

    while (off < list_end && cert_count < X509_MAX_CHAIN_CERTS){
        if ((off + 3u) > list_end){
            return -1;
        }
        unsigned int dlen = be24_read(&cert_body[off]);
        off += 3u;
        if (dlen == 0u || (off + dlen) > list_end){
            return -1;
        }
        cert_der[cert_count] = &cert_body[off];
        cert_der_len[cert_count] = dlen;
        off += dlen;

        if ((off + 2u) > list_end){
            return -1;
        }
        unsigned int ext_len = ((unsigned int)cert_body[off] << 8) | (unsigned int)cert_body[off + 1u];
        off += 2u;
        if ((off + ext_len) > list_end){
            return -1;
        }
        off += ext_len;
        cert_count++;
    }
    if (off != list_end || cert_count == 0u){
        return -1;
    }
    explicit_policy = cert_count + 1u;
    inhibit_any_policy = cert_count + 1u;
    policy_mapping = cert_count + 1u;

    for (unsigned int i = 0u; i < cert_count; i++){
        int leaf = (i == 0u) ? 1 : 0;
        if (parse_cert_identity(cert_der[i], cert_der_len[i], host, host_len, leaf, &certs[i]) != 0){
            return -1;
        }
        if (name_hash(certs[i].subject_tlv, certs[i].subject_tlv_len, subject_hashes[i]) != 0 ||
            name_hash(certs[i].issuer_tlv, certs[i].issuer_tlv_len, issuer_hashes[i]) != 0){
            return -1;
        }
        if (certs[i].unknown_critical_ext){
            uart_puts("X509: unknown critical extension.\n");
            return -1;
        }
        if (certs[i].name_constraints_present &&
            certs[i].name_constraints_critical &&
            certs[i].name_constraints_parse_error){
            uart_puts("X509: critical nameConstraints parse failed.\n");
            return -1;
        }
        if (cert_time_valid_now(&certs[i], now_unix) != 0){
            uart_puts("X509: certificate time invalid.\n");
            return -1;
        }
        used[i] = 0;
    }

    if (!certs[0].hostname_match){
        uart_puts("X509: hostname mismatch.\n");
        return -1;
    }
    if (certs[0].key_usage_present &&
        (certs[0].key_usage_bits & X509_KU_DIGITAL_SIGNATURE) == 0u){
        uart_puts("X509: leaf keyUsage missing digitalSignature.\n");
        return -1;
    }
    if (certs[0].eku_present &&
        !(certs[0].eku_server_auth || certs[0].eku_any)){
        uart_puts("X509: leaf EKU missing serverAuth.\n");
        return -1;
    }

    // Build a chain path from leaf (index 0), tolerating out-of-order
    // intermediates and extra certificates. Leaf must be first.
    unsigned int cur = 0u;
    used[cur] = 1;
    path_idx[path_len++] = cur;
    for (unsigned int depth = 0u; depth < cert_count; depth++){
        if (cert_der_hash(cert_der[cur], cert_der_len[cur], cur_der_hash) != 0){
            return -1;
        }
        // Trust anchor may be sent directly in the TLS chain.
        if (anchor_contains_der_hash(cur_der_hash)){
            anchor_ok = 1;
            anchor_in_chain = 1;
            break;
        }

        // More commonly, the root is omitted. In that case the current
        // certificate must be signed by the trusted root public key.
        anchor_idx = anchor_find_subject_hash(issuer_hashes[cur]);
        if (anchor_idx >= 0){
            unsigned long t0 = system_ticks;
            if (verify_cert_signed_by_anchor(&certs[cur], &g_ca_anchors[(unsigned int)anchor_idx]) != 0){
                uart_puts("X509: root signature verify failed alg=");
                uart_puthex((unsigned int)certs[cur].cert_sig_alg);
                uart_puts("\n");
                return -1;
            }
            if ((long)(system_ticks - t0) > 250){
                uart_puts("X509: root signature verify ms=");
                uart_putdec((unsigned long)(system_ticks - t0));
                uart_puts(" alg=");
                uart_puthex((unsigned int)certs[cur].cert_sig_alg);
                uart_puts("\n");
            }
            if (!anchor_is_ca_usable(&g_ca_anchors[(unsigned int)anchor_idx])){
                uart_puts("X509: anchor CA constraints invalid.\n");
                return -1;
            }
            anchor_ok = 1;
            anchor_in_chain = 0;
            break;
        }

        int next = -1;
        for (unsigned int j = 1u; j < cert_count; j++){
            if (used[j]){
                continue;
            }
            if (crypto_consttime_equal(subject_hashes[j], issuer_hashes[cur], 32u)){
                next = (int)j;
                break;
            }
        }
        if (next < 0){
            break;
        }
        if (!cert_is_ca_usable(&certs[(unsigned int)next])){
            uart_puts("X509: intermediate CA constraints invalid.\n");
            return -1;
        }
        unsigned long t0 = system_ticks;
        if (verify_cert_signed_by_cert(&certs[cur], &certs[(unsigned int)next]) != 0){
            uart_puts("X509: chain signature verify failed alg=");
            uart_puthex((unsigned int)certs[cur].cert_sig_alg);
            uart_puts("\n");
            return -1;
        }
        if ((long)(system_ticks - t0) > 250){
            uart_puts("X509: chain signature verify ms=");
            uart_putdec((unsigned long)(system_ticks - t0));
            uart_puts(" alg=");
            uart_puthex((unsigned int)certs[cur].cert_sig_alg);
            uart_puts("\n");
        }
        cur = (unsigned int)next;
        used[cur] = 1;
        if (path_len >= X509_MAX_CHAIN_CERTS){
            return -1;
        }
        path_idx[path_len++] = cur;
    }
    if (!anchor_ok){
        uart_puts("X509: chain not anchored in CA_ROOTS.\n");
        return -1;
    }

    // Enforce path length constraints from each in-chain CA cert.
    // path_idx[0] is the leaf; all later certificates are CAs.
    for (unsigned int i = 1u; i < path_len; i++){
        const parsed_cert_t* ca = &certs[path_idx[i]];
        unsigned int ca_below = 0u; // non-self-issued CA certs between leaf and this CA
        for (unsigned int k = 1u; k < i; k++){
            const parsed_cert_t* lower = &certs[path_idx[k]];
            if (!cert_is_self_issued(lower)){
                ca_below++;
            }
        }
        if (leaf_host_allowed_by_name_constraints(ca, host, host_len) != 0){
            uart_puts("X509: nameConstraints reject leaf host.\n");
            return -1;
        }
        if (ca->path_len_present && ca_below > ca->path_len_constraint){
            uart_puts("X509: pathLen constraint violated (chain cert).\n");
            return -1;
        }
    }
    if (!anchor_in_chain && anchor_idx >= 0){
        unsigned int ca_below_anchor = 0u;
        const ca_anchor_t* a = &g_ca_anchors[(unsigned int)anchor_idx];
        for (unsigned int k = 1u; k < path_len; k++){
            const parsed_cert_t* lower = &certs[path_idx[k]];
            if (!cert_is_self_issued(lower)){
                ca_below_anchor++;
            }
        }
        if (a->path_len_present && ca_below_anchor > a->path_len_constraint){
            uart_puts("X509: pathLen constraint violated (anchor).\n");
            return -1;
        }
    }

    // Revocation denylist check for each chain cert presented by peer.
    for (unsigned int i = 0u; i < path_len; i++){
        unsigned int ci = path_idx[i];
        if (cert_is_revoked(issuer_hashes[ci], &certs[ci])){
            uart_puts("X509: certificate revoked by local policy.\n");
            return -1;
        }
    }

    // Policy-state processing (compact RFC5280-style counters).
    // Traverse from trust side to leaf (path_idx[path_len-1] .. path_idx[0]).
    for (unsigned int ridx = path_len; ridx > 0u; ridx--){
        unsigned int ci = path_idx[ridx - 1u];
        const parsed_cert_t* c = &certs[ci];
        int is_leaf = (ridx == 1u) ? 1 : 0;
        int self_issued = cert_is_self_issued(c);

        if (policy_mapping == 0u && c->policy_mappings_present){
            uart_puts("X509: policyMappings inhibited.\n");
            return -1;
        }
        if (explicit_policy == 0u && !c->cert_policies_present){
            uart_puts("X509: explicit policy required but missing certificatePolicies.\n");
            return -1;
        }
        if (inhibit_any_policy == 0u &&
            c->cert_policies_present && c->cert_policy_any &&
            !(self_issued && !is_leaf)){
            uart_puts("X509: anyPolicy inhibited.\n");
            return -1;
        }

        if (!self_issued){
            if (explicit_policy > 0u){
                explicit_policy--;
            }
            if (policy_mapping > 0u){
                policy_mapping--;
            }
            if (inhibit_any_policy > 0u){
                inhibit_any_policy--;
            }
        }

        if (c->policy_constraints_req_exp_present &&
            c->policy_constraints_req_exp < explicit_policy){
            explicit_policy = c->policy_constraints_req_exp;
        }
        if (c->policy_constraints_inhibit_map_present &&
            c->policy_constraints_inhibit_map < policy_mapping){
            policy_mapping = c->policy_constraints_inhibit_map;
        }
        if (c->inhibit_any_policy_present &&
            c->inhibit_any_policy_skip < inhibit_any_policy){
            inhibit_any_policy = c->inhibit_any_policy_skip;
        }
    }

    if (out_result){
        out_result->chain_certs = path_len;
        out_result->anchor_count = g_ca_anchor_count;
        out_result->leaf_cert_sig_alg = certs[0].cert_sig_alg;
        out_result->leaf_key_alg = X509_VERIFY_KEY_NONE;
        out_result->leaf_rsa_n_len = certs[0].rsa_n_len;
        out_result->leaf_rsa_e_len = certs[0].rsa_e_len;
        out_result->leaf_ec_curve = certs[0].ec_curve;
        out_result->leaf_ec_q_len = certs[0].ec_q_len;
        if (certs[0].key_alg == X509_KEY_ALG_RSA){
            out_result->leaf_key_alg = X509_VERIFY_KEY_RSA;
        } else if (certs[0].key_alg == X509_KEY_ALG_EC){
            out_result->leaf_key_alg = X509_VERIFY_KEY_EC;
        }
        for (unsigned int i = 0; i < certs[0].rsa_n_len; i++){
            out_result->leaf_rsa_n[i] = certs[0].rsa_n[i];
        }
        for (unsigned int i = 0; i < certs[0].rsa_e_len; i++){
            out_result->leaf_rsa_e[i] = certs[0].rsa_e[i];
        }
        for (unsigned int i = 0; i < certs[0].ec_q_len; i++){
            out_result->leaf_ec_qx[i] = certs[0].ec_qx[i];
            out_result->leaf_ec_qy[i] = certs[0].ec_qy[i];
        }
        out_result->hostname_ok = certs[0].hostname_match ? 1 : 0;
        out_result->chain_anchor_ok = anchor_ok ? 1 : 0;
    }

    (void)server_cert_verify_alg;
    return 0;
}
