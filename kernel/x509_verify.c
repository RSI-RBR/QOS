#include "x509_verify.h"
#include "fat32.h"
#include "sha256.h"
#include "trust.h"
#include "program.h"
#include "pq_sig.h"
#include "ed25519_verify.h"
#include "crypto.h"
#include "string.h"
#include "uart.h"

#define X509_MAX_CHAIN_CERTS 8u
#define X509_MAX_CA_ANCHORS 256u
#define X509_CA_PEM_MAX (512u * 1024u)
#define X509_CA_SIG_MAX 128u
#define X509_CA_LABEL "CA_ROOTS_PEM"
#define X509_CA_LABEL_LEN 12u

#define X509_CA_PEM_FAT "CA_ROOTS PEM"
#define X509_CA_SIG_FAT "CA_ROOTS SIG"
#define X509_CA_PQS_FAT "CA_ROOTS PQS"

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
    unsigned short cert_sig_alg;
    int san_present;
    int hostname_match;
} parsed_cert_t;

typedef struct {
    unsigned char subject_hash[32];
    unsigned char der_hash[32];
} ca_anchor_t;

static unsigned char g_ca_pem_buf[X509_CA_PEM_MAX + 1u];
static unsigned char g_ca_sig_buf[X509_CA_SIG_MAX];
static unsigned char g_ca_pqs_buf[QOS_PQ_SIG_MAX];
static unsigned char g_ca_der_tmp[8192];
static ca_anchor_t g_ca_anchors[X509_MAX_CA_ANCHORS];
static unsigned int g_ca_anchor_count = 0u;
static int g_ca_cache_state = 0; // 0=uninitialized, 1=ready, -1=failed

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

static int parse_cert_leaf_hostname(const unsigned char* tbs_tlv,
                                    unsigned int tbs_tlv_len,
                                    const char* host,
                                    unsigned int host_len,
                                    int* out_san_present,
                                    int* out_hostname_match,
                                    const unsigned char** out_subject_tlv,
                                    unsigned int* out_subject_tlv_len,
                                    const unsigned char** out_issuer_tlv,
                                    unsigned int* out_issuer_tlv_len){
    static const unsigned char OID_SAN[] = { 0x55,0x1D,0x11 };
    asn1_tlv_t tbs;
    unsigned int off;

    if (!tbs_tlv || tbs_tlv_len == 0u || !host || host_len == 0u ||
        !out_san_present || !out_hostname_match ||
        !out_subject_tlv || !out_subject_tlv_len ||
        !out_issuer_tlv || !out_issuer_tlv_len){
        return -1;
    }
    *out_san_present = 0;
    *out_hostname_match = 0;

    if (asn1_parse_tlv(tbs_tlv, tbs_tlv_len, &tbs) != 0 || tbs.tag != 0x30u){
        return -1;
    }
    off = 0u;

    // Optional version [0] EXPLICIT
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
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &sn) != 0 || sn.tag != 0x02u){
            return -1;
        }
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
        *out_issuer_tlv = issuer.hdr;
        *out_issuer_tlv_len = issuer.total_len;
        off += issuer.total_len;
    }
    // validity
    {
        asn1_tlv_t validity;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &validity) != 0 || validity.tag != 0x30u){
            return -1;
        }
        off += validity.total_len;
    }
    // subject Name
    {
        asn1_tlv_t subject;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &subject) != 0 || subject.tag != 0x30u){
            return -1;
        }
        *out_subject_tlv = subject.hdr;
        *out_subject_tlv_len = subject.total_len;
        off += subject.total_len;
    }
    // subjectPublicKeyInfo
    {
        asn1_tlv_t spki;
        if (asn1_parse_tlv(tbs.val + off, tbs.val_len - off, &spki) != 0 || spki.tag != 0x30u){
            return -1;
        }
        off += spki.total_len;
    }

    int has_dns_san = 0;
    int match = 0;

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
            if (asn1_parse_tlv(exts.val + eoff, exts.val_len - eoff, &ext) != 0 || ext.tag != 0x30u){
                return -1;
            }
            eoff += ext.total_len;

            unsigned int xoff = 0u;
            asn1_tlv_t oid;
            if (asn1_parse_tlv(ext.val + xoff, ext.val_len - xoff, &oid) != 0 || oid.tag != 0x06u){
                return -1;
            }
            xoff += oid.total_len;

            // optional critical boolean
            if (xoff < ext.val_len && ext.val[xoff] == 0x01u){
                asn1_tlv_t critical;
                if (asn1_parse_tlv(ext.val + xoff, ext.val_len - xoff, &critical) != 0 || critical.tag != 0x01u){
                    return -1;
                }
                xoff += critical.total_len;
            }

            if (xoff >= ext.val_len){
                return -1;
            }
            asn1_tlv_t ext_value;
            if (asn1_parse_tlv(ext.val + xoff, ext.val_len - xoff, &ext_value) != 0 || ext_value.tag != 0x04u){
                return -1;
            }

            if (oid_equal(&oid, OID_SAN, sizeof(OID_SAN))){
                int san_match = parse_san_dns_match(ext_value.val, ext_value.val_len,
                                                    host, host_len, &has_dns_san);
                if (san_match){
                    match = 1;
                }
            }
        }
    }

    if (!has_dns_san){
        match = parse_subject_cn_match(*out_subject_tlv, *out_subject_tlv_len, host, host_len);
    }
    *out_san_present = has_dns_san;
    *out_hostname_match = match;
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

    if (!der || der_len == 0u || !out){
        return -1;
    }
    out->subject_tlv = 0;
    out->subject_tlv_len = 0;
    out->issuer_tlv = 0;
    out->issuer_tlv_len = 0;
    out->cert_sig_alg = 0u;
    out->san_present = 0;
    out->hostname_match = 0;

    if (asn1_parse_tlv(der, der_len, &cert) != 0 || cert.tag != 0x30u || cert.total_len != der_len){
        return -1;
    }

    if (asn1_parse_tlv(cert.val + off, cert.val_len - off, &tbs) != 0 || tbs.tag != 0x30u){
        return -1;
    }
    off += tbs.total_len;

    if (asn1_parse_tlv(cert.val + off, cert.val_len - off, &sigalg) != 0 || sigalg.tag != 0x30u){
        return -1;
    }
    off += sigalg.total_len;
    (void)off;

    (void)parse_signature_algorithm(&sigalg, &out->cert_sig_alg);

    if (leaf){
        if (parse_cert_leaf_hostname(tbs.hdr, tbs.total_len, host, host_len,
                                     &out->san_present,
                                     &out->hostname_match,
                                     &out->subject_tlv, &out->subject_tlv_len,
                                     &out->issuer_tlv, &out->issuer_tlv_len) != 0){
            return -1;
        }
    } else{
        // Parse only issuer/subject names for chain and anchor matching.
        const char dummy_host[] = "x";
        int san_p = 0;
        int hn_m = 0;
        if (parse_cert_leaf_hostname(tbs.hdr, tbs.total_len,
                                     dummy_host, 1u,
                                     &san_p, &hn_m,
                                     &out->subject_tlv, &out->subject_tlv_len,
                                     &out->issuer_tlv, &out->issuer_tlv_len) != 0){
            return -1;
        }
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
        g_ca_cache_state = -1;
        return -1;
    }
    if (n >= (int)X509_CA_PEM_MAX){
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
        g_ca_cache_state = -1;
        return -1;
    }
    g_ca_cache_state = 1;
    return 0;
}

static int anchor_contains_subject_hash(const unsigned char subject_hash[32]){
    for (unsigned int i = 0; i < g_ca_anchor_count; i++){
        if (crypto_consttime_equal(g_ca_anchors[i].subject_hash, subject_hash, 32u)){
            return 1;
        }
    }
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

void x509_verify_reset_cache(void){
    g_ca_anchor_count = 0u;
    g_ca_cache_state = 0;
    crypto_memzero(g_ca_pem_buf, sizeof(g_ca_pem_buf));
    crypto_memzero(g_ca_sig_buf, sizeof(g_ca_sig_buf));
    crypto_memzero(g_ca_pqs_buf, sizeof(g_ca_pqs_buf));
    crypto_memzero(g_ca_anchors, sizeof(g_ca_anchors));
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
    unsigned int path_len = 0u;

    if (out_result){
        out_result->chain_certs = 0u;
        out_result->anchor_count = g_ca_anchor_count;
        out_result->leaf_cert_sig_alg = 0u;
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

    if (ensure_ca_anchors_loaded() != 0){
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

    for (unsigned int i = 0u; i < cert_count; i++){
        int leaf = (i == 0u) ? 1 : 0;
        if (parse_cert_identity(cert_der[i], cert_der_len[i], host, host_len, leaf, &certs[i]) != 0){
            return -1;
        }
        if (name_hash(certs[i].subject_tlv, certs[i].subject_tlv_len, subject_hashes[i]) != 0 ||
            name_hash(certs[i].issuer_tlv, certs[i].issuer_tlv_len, issuer_hashes[i]) != 0){
            return -1;
        }
        used[i] = 0;
    }

    if (!certs[0].hostname_match){
        uart_puts("X509: hostname mismatch.\n");
        return -1;
    }

    // Build a chain path from leaf (index 0), tolerating out-of-order
    // intermediates and extra certificates. Leaf must be first.
    unsigned int cur = 0u;
    used[cur] = 1;
    path_len = 1u;
    int anchor_ok = 0;
    for (unsigned int depth = 0u; depth < cert_count; depth++){
        if (cert_der_hash(cert_der[cur], cert_der_len[cur], cur_der_hash) != 0){
            return -1;
        }
        // Trust anchor may be sent directly, or omitted (issuer name match).
        if (anchor_contains_der_hash(cur_der_hash) ||
            anchor_contains_subject_hash(issuer_hashes[cur])){
            anchor_ok = 1;
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
        cur = (unsigned int)next;
        used[cur] = 1;
        path_len++;
    }
    if (!anchor_ok){
        uart_puts("X509: chain not anchored in CA_ROOTS.\n");
        return -1;
    }

    if (out_result){
        out_result->chain_certs = path_len;
        out_result->anchor_count = g_ca_anchor_count;
        out_result->leaf_cert_sig_alg = certs[0].cert_sig_alg;
        out_result->hostname_ok = certs[0].hostname_match ? 1 : 0;
        out_result->chain_anchor_ok = anchor_ok ? 1 : 0;
    }

    (void)server_cert_verify_alg;
    return 0;
}
