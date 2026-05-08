#include "syscall.h"
#include "user_net.h"

#define INPUT_CAP 192
#define REQ_CAP 1024
#define RESP_CAP 524288
#define DNS_TIMEOUT_MS 3000u
#define RECV_TIMEOUT_MS 3000u
#define HOST_CAP 128
#define PATH_CAP 256
#define URL_CAP 384
#define MAX_REDIRECTS 4

static char g_input[INPUT_CAP];
static int g_input_len = 0;
static int g_accept_gzip = 0;

static const unsigned char g_dns_server[4] = {10, 0, 0, 1};

// Forward declarations for helpers used before their definitions.
static unsigned char ascii_lower(unsigned char c);
static int append_str(char* dst, int cap, int* idx, const char* s);
static int hex_value(unsigned char c);

static void print_uint(unsigned int v){
    char tmp[16];
    int n = 0;
    if (v == 0){
        qos_putc('0');
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0){
        qos_putc(tmp[--n]);
    }
}

static void print_int(int v){
    if (v < 0){
        qos_putc('-');
        print_uint((unsigned int)(-(long long)v));
        return;
    }
    print_uint((unsigned int)v);
}

static void print_ip4(const unsigned char ip[4]){
    print_uint((unsigned int)ip[0]);
    qos_putc('.');
    print_uint((unsigned int)ip[1]);
    qos_putc('.');
    print_uint((unsigned int)ip[2]);
    qos_putc('.');
    print_uint((unsigned int)ip[3]);
}

static void print_fetch_timing(unsigned long dns_ms,
                               unsigned long connect_ms,
                               unsigned long send_ms,
                               unsigned long first_chunk_ms,
                               unsigned long recv_ms,
                               unsigned int recv_chunks,
                               unsigned long total_ms){
    qos_puts("Fetch timing ms: dns=");
    print_uint((unsigned int)dns_ms);
    qos_puts(" connect=");
    print_uint((unsigned int)connect_ms);
    qos_puts(" send_blocking=");
    print_uint((unsigned int)send_ms);
    qos_puts(" first_chunk=");
    print_uint((unsigned int)first_chunk_ms);
    qos_puts(" recv=");
    print_uint((unsigned int)recv_ms);
    qos_puts(" recv_chunks=");
    print_uint(recv_chunks);
    qos_puts(" total=");
    print_uint((unsigned int)total_ms);
    qos_puts("\n");
}

static int str_eq(const char* a, const char* b){
    while (*a && *b){
        if (*a != *b){
            return 0;
        }
        a++;
        b++;
    }
    return (*a == 0 && *b == 0);
}

static int str_starts_with(const char* s, const char* prefix){
    while (*prefix){
        if (*s != *prefix){
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

static int copy_cstr(char* dst, int cap, const char* src){
    int i = 0;
    if (!dst || cap <= 0 || !src){
        return -1;
    }
    while (src[i]){
        if (i >= (cap - 1)){
            return -1;
        }
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return 0;
}

static int str_eq_ci_n(const unsigned char* a, const char* b, int n){
    int i = 0;
    if (!a || !b || n < 0){
        return 0;
    }
    while (i < n && b[i]){
        if (ascii_lower(a[i]) != ascii_lower((unsigned char)b[i])){
            return 0;
        }
        i++;
    }
    return (i == n && b[i] == 0);
}

static int parse_http_status_code(const unsigned char* resp, int len){
    int i = 0;
    if (!resp || len <= 0){
        return 0;
    }
    while (i < len && resp[i] != ' ' && resp[i] != '\r' && resp[i] != '\n'){
        i++;
    }
    while (i < len && resp[i] == ' '){
        i++;
    }
    if (i + 2 >= len){
        return 0;
    }
    if (resp[i] < '0' || resp[i] > '9' ||
        resp[i + 1] < '0' || resp[i + 1] > '9' ||
        resp[i + 2] < '0' || resp[i + 2] > '9'){
        return 0;
    }
    return (resp[i] - '0') * 100 + (resp[i + 1] - '0') * 10 + (resp[i + 2] - '0');
}

static int extract_header_value(const unsigned char* resp, int len, const char* header_name,
                                char* out, int out_cap){
    int i = 0;
    int name_len = 0;
    if (!resp || len <= 0 || !header_name || !out || out_cap <= 1){
        return -1;
    }
    while (header_name[name_len]){
        name_len++;
    }

    // Skip status line.
    while (i < len && resp[i] != '\n'){
        i++;
    }
    if (i < len){
        i++;
    }

    while (i < len){
        int ls = i;
        int le = ls;
        int next;
        int colon = -1;
        int vs, ve, n, w;

        while (le < len && resp[le] != '\n'){
            if (colon < 0 && resp[le] == ':'){
                colon = le;
            }
            le++;
        }
        next = (le < len) ? (le + 1) : le;

        while (le > ls && (resp[le - 1] == '\r' || resp[le - 1] == '\n')){
            le--;
        }
        if (le == ls){
            break;
        }

        if (colon > ls && str_eq_ci_n(&resp[ls], header_name, colon - ls)){
            vs = colon + 1;
            while (vs < le && (resp[vs] == ' ' || resp[vs] == '\t')){
                vs++;
            }
            ve = le;
            while (ve > vs && (resp[ve - 1] == ' ' || resp[ve - 1] == '\t')){
                ve--;
            }
            n = ve - vs;
            if (n <= 0){
                return -1;
            }
            if (n > out_cap - 1){
                n = out_cap - 1;
            }
            for (w = 0; w < n; w++){
                out[w] = (char)resp[vs + w];
            }
            out[n] = 0;
            return 0;
        }

        i = next;
    }
    return -1;
}

static int parse_url_target(const char* in, int default_https,
                            int* out_https, char* out_host, int host_cap,
                            char* out_path, int path_cap){
    const char* p;
    int https;
    int hi = 0;
    int pi = 0;
    int colon_idx = -1;

    if (!in || !*in || !out_https || !out_host || host_cap < 2 || !out_path || path_cap < 2){
        return -1;
    }

    p = in;
    https = default_https ? 1 : 0;
    if (str_starts_with(p, "https://")){
        https = 1;
        p += 8;
    } else if (str_starts_with(p, "http://")){
        https = 0;
        p += 7;
    } else if (str_starts_with(p, "//")){
        p += 2;
    }

    while (*p && *p != '/'){
        if (hi >= host_cap - 1){
            return -1;
        }
        if (*p == ':' && colon_idx < 0){
            colon_idx = hi;
        }
        out_host[hi++] = *p++;
    }
    out_host[hi] = 0;
    if (hi == 0){
        return -1;
    }

    if (colon_idx >= 0){
        int port = 0;
        int j = colon_idx + 1;
        if (j >= hi){
            return -1;
        }
        while (j < hi){
            char c = out_host[j++];
            if (c < '0' || c > '9'){
                return -1;
            }
            port = (port * 10) + (c - '0');
        }
        out_host[colon_idx] = 0;
        if (port == 443){
            https = 1;
        } else if (port == 80){
            https = 0;
        }
    }

    if (*p == 0){
        out_path[0] = '/';
        out_path[1] = 0;
    } else{
        while (*p){
            if (pi >= path_cap - 1){
                return -1;
            }
            out_path[pi++] = *p++;
        }
        out_path[pi] = 0;
    }

    *out_https = https;
    return 0;
}

static int resolve_redirect_target(const char* location,
                                   int cur_https,
                                   const char* cur_host,
                                   const char* cur_path,
                                   int* out_https,
                                   char* out_host, int host_cap,
                                   char* out_path, int path_cap){
    char tmp[URL_CAP];
    int k = 0;

    if (!location || !*location || !cur_host || !*cur_host ||
        !cur_path || !*cur_path || !out_https || !out_host || !out_path){
        return -1;
    }

    if (str_starts_with(location, "https://") ||
        str_starts_with(location, "http://") ||
        str_starts_with(location, "//")){
        return parse_url_target(location, cur_https, out_https, out_host, host_cap, out_path, path_cap);
    }

    if (location[0] == '/'){
        if (copy_cstr(out_host, host_cap, cur_host) != 0 ||
            copy_cstr(out_path, path_cap, location) != 0){
            return -1;
        }
        *out_https = cur_https;
        return 0;
    }

    // Relative location: resolve against current directory.
    while (cur_path[k] && k < (URL_CAP - 1)){
        tmp[k] = cur_path[k];
        k++;
    }
    tmp[k] = 0;

    if (k == 0 || tmp[0] != '/'){
        tmp[0] = '/';
        tmp[1] = 0;
        k = 1;
    }

    while (k > 0 && tmp[k - 1] != '/'){
        k--;
    }
    tmp[k] = 0;

    if (append_str(tmp, URL_CAP, &k, location) != 0){
        return -1;
    }

    if (copy_cstr(out_host, host_cap, cur_host) != 0 ||
        copy_cstr(out_path, path_cap, tmp) != 0){
        return -1;
    }
    *out_https = cur_https;
    return 0;
}

static int append_char(char* dst, int cap, int* idx, char c){
    if (*idx >= cap - 1){
        return -1;
    }
    dst[*idx] = c;
    (*idx)++;
    dst[*idx] = 0;
    return 0;
}

static int append_str(char* dst, int cap, int* idx, const char* s){
    while (*s){
        if (append_char(dst, cap, idx, *s++) != 0){
            return -1;
        }
    }
    return 0;
}

static int find_http_body(const unsigned char* resp, int len){
    for (int i = 0; i + 3 < len; i++){
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n'){
            return i + 4;
        }
    }
    return 0;
}

static int http_header_name_eq_local(const unsigned char* p, int len, const char* name){
    int nlen = 0;
    if (!p || len <= 0 || !name){
        return 0;
    }
    while (name[nlen]){
        nlen++;
    }
    if (len != nlen){
        return 0;
    }
    for (int i = 0; i < len; i++){
        if (ascii_lower(p[i]) != ascii_lower((unsigned char)name[i])){
            return 0;
        }
    }
    return 1;
}

static int http_value_has_token_local(const unsigned char* p, int len, const char* token){
    int tlen = 0;
    if (!p || len <= 0 || !token){
        return 0;
    }
    while (token[tlen]){
        tlen++;
    }
    if (tlen == 0 || len < tlen){
        return 0;
    }
    for (int i = 0; i + tlen <= len; i++){
        int j = 0;
        while (j < tlen && ascii_lower(p[i + j]) == ascii_lower((unsigned char)token[j])){
            j++;
        }
        if (j == tlen){
            return 1;
        }
    }
    return 0;
}

static int http_chunked_complete_local(const unsigned char* body, int len){
    int i = 0;
    if (!body || len < 0){
        return 0;
    }

    while (i < len){
        unsigned int chunk_len = 0u;
        int have_hex = 0;

        while (i < len && (body[i] == '\r' || body[i] == '\n')){
            i++;
        }
        if (i >= len){
            return 0;
        }

        while (i < len){
            int hv = hex_value(body[i]);
            if (hv >= 0){
                if (chunk_len > 0x0FFFFFFFu){
                    return -1;
                }
                chunk_len = (chunk_len << 4) | (unsigned int)hv;
                have_hex = 1;
                i++;
                continue;
            }
            if (body[i] == ';'){
                while (i < len && body[i] != '\n'){
                    i++;
                }
                break;
            }
            if (body[i] == '\r' || body[i] == '\n'){
                break;
            }
            return -1;
        }
        if (!have_hex){
            return -1;
        }

        while (i < len && body[i] != '\n'){
            i++;
        }
        if (i >= len){
            return 0;
        }
        i++; // consume '\n'

        if (chunk_len == 0u){
            // Trailer section ends with an empty line.
            while (1){
                int line_start = i;
                while (i < len && body[i] != '\n'){
                    i++;
                }
                if (i >= len){
                    return 0;
                }
                i++; // consume '\n'
                if (i - line_start <= 2){
                    return 1;
                }
            }
        }

        if ((unsigned int)(len - i) < chunk_len + 2u){
            return 0;
        }
        i += (int)chunk_len;
        if (body[i] != '\r' || body[i + 1] != '\n'){
            return -1;
        }
        i += 2;
    }

    return 0;
}

static int http_response_completion_state_local(const unsigned char* buf, int len){
    int body = 0;
    int i = 0;
    unsigned int content_len = 0u;
    int have_content_len = 0;
    int chunked = 0;

    if (!buf || len <= 0){
        return 0;
    }

    body = find_http_body(buf, len);
    if (body <= 0){
        return 0;
    }

    while (i < body){
        int ls = i;
        int le;
        int colon = -1;
        int vs;
        int ve;

        while (i < body && buf[i] != '\n'){
            if (buf[i] == ':' && colon < 0){
                colon = i;
            }
            i++;
        }
        le = i;
        if (i < body && buf[i] == '\n'){
            i++;
        }
        while (le > ls && (buf[le - 1] == '\r' || buf[le - 1] == '\n')){
            le--;
        }
        if (le == ls){
            break;
        }
        if (colon < 0 || colon <= ls || colon >= le){
            continue;
        }

        vs = colon + 1;
        while (vs < le && (buf[vs] == ' ' || buf[vs] == '\t')){
            vs++;
        }
        ve = le;
        while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t')){
            ve--;
        }

        if (http_header_name_eq_local(&buf[ls], colon - ls, "content-length")){
            unsigned int v = 0u;
            int ok = 0;
            for (int k = vs; k < ve; k++){
                if (buf[k] < '0' || buf[k] > '9'){
                    ok = 0;
                    break;
                }
                ok = 1;
                v = (v * 10u) + (unsigned int)(buf[k] - '0');
            }
            if (ok){
                content_len = v;
                have_content_len = 1;
            }
        } else if (http_header_name_eq_local(&buf[ls], colon - ls, "transfer-encoding") &&
                   http_value_has_token_local(&buf[vs], ve - vs, "chunked")){
            chunked = 1;
        }
    }

    if (have_content_len){
        return ((unsigned int)(len - body) >= content_len) ? 1 : 0;
    }
    if (chunked){
        int c = http_chunked_complete_local(&buf[body], len - body);
        if (c == 1){
            return 1;
        }
        return 0;
    }
    return -1;
}

static int str_contains_ci(const char* haystack, const char* needle){
    int needle_len = 0;
    if (!haystack || !needle || !*needle){
        return 0;
    }
    while (needle[needle_len]){
        needle_len++;
    }
    for (int i = 0; haystack[i]; i++){
        int j = 0;
        while (j < needle_len && haystack[i + j] &&
               ascii_lower((unsigned char)haystack[i + j]) == ascii_lower((unsigned char)needle[j])){
            j++;
        }
        if (j == needle_len){
            return 1;
        }
    }
    return 0;
}

static int hex_value(unsigned char c){
    if (c >= '0' && c <= '9'){
        return (int)(c - '0');
    }
    c = ascii_lower(c);
    if (c >= 'a' && c <= 'f'){
        return (int)(10 + c - 'a');
    }
    return -1;
}

typedef struct {
    const unsigned char* in;
    int in_len;
    int in_pos;
    unsigned int bitbuf;
    int bitcnt;
} inflate_reader_t;

typedef struct {
    unsigned short count[16];
    unsigned short symbol[320];
} inflate_huff_t;

static int inflate_bits(inflate_reader_t* r, int n, unsigned int* out){
    if (!r || !out || n < 0 || n > 16){
        return -1;
    }
    while (r->bitcnt < n){
        if (r->in_pos >= r->in_len){
            return -1;
        }
        r->bitbuf |= ((unsigned int)r->in[r->in_pos++]) << r->bitcnt;
        r->bitcnt += 8;
    }
    if (n == 0){
        *out = 0u;
        return 0;
    }
    *out = r->bitbuf & ((1u << n) - 1u);
    r->bitbuf >>= n;
    r->bitcnt -= n;
    return 0;
}

static void inflate_align_byte(inflate_reader_t* r){
    int drop;
    if (!r){
        return;
    }
    drop = r->bitcnt & 7;
    if (drop > 0){
        r->bitbuf >>= drop;
        r->bitcnt -= drop;
    }
}

static int inflate_huff_build(inflate_huff_t* h, const unsigned char* lens, int n){
    unsigned short offs[17];
    int left = 1;
    int i;

    if (!h || !lens || n < 0 || n > 320){
        return -1;
    }
    for (i = 0; i <= 15; i++){
        h->count[i] = 0;
    }
    for (i = 0; i < n; i++){
        if (lens[i] > 15u){
            return -1;
        }
        h->count[lens[i]]++;
    }
    h->count[0] = 0;
    for (i = 1; i <= 15; i++){
        left <<= 1;
        left -= (int)h->count[i];
        if (left < 0){
            return -1;
        }
    }

    offs[1] = 0;
    for (i = 1; i < 16; i++){
        offs[i + 1] = (unsigned short)(offs[i] + h->count[i]);
    }
    for (i = 0; i < n; i++){
        unsigned int len = lens[i];
        if (len != 0u){
            h->symbol[offs[len]++] = (unsigned short)i;
        }
    }
    return 0;
}

static int inflate_huff_decode(inflate_reader_t* r, const inflate_huff_t* h, int* sym){
    unsigned int code = 0u;
    unsigned int first = 0u;
    unsigned int index = 0u;
    int len;

    if (!r || !h || !sym){
        return -1;
    }
    for (len = 1; len <= 15; len++){
        unsigned int bit = 0u;
        unsigned int count;
        if (inflate_bits(r, 1, &bit) != 0){
            return -1;
        }
        code |= (bit << (len - 1));
        count = (unsigned int)h->count[len];
        if (code < first + count){
            *sym = (int)h->symbol[index + (code - first)];
            return 0;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_build_fixed(inflate_huff_t* ll, inflate_huff_t* dd){
    unsigned char ll_lens[288];
    unsigned char dd_lens[32];
    int i;
    for (i = 0; i <= 143; i++) ll_lens[i] = 8;
    for (i = 144; i <= 255; i++) ll_lens[i] = 9;
    for (i = 256; i <= 279; i++) ll_lens[i] = 7;
    for (i = 280; i <= 287; i++) ll_lens[i] = 8;
    for (i = 0; i < 32; i++) dd_lens[i] = 5;
    if (inflate_huff_build(ll, ll_lens, 288) != 0){
        return -1;
    }
    if (inflate_huff_build(dd, dd_lens, 32) != 0){
        return -1;
    }
    return 0;
}

static int inflate_raw_deflate_local(const unsigned char* in, int in_len, unsigned char* out, int out_cap){
    static const unsigned short len_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const unsigned char len_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const unsigned short dist_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const unsigned char dist_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    static const unsigned char order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    inflate_reader_t r;
    inflate_huff_t ll;
    inflate_huff_t dd;
    int out_pos = 0;
    int last = 0;

    if (!in || in_len <= 0 || !out || out_cap <= 0){
        return -1;
    }
    r.in = in;
    r.in_len = in_len;
    r.in_pos = 0;
    r.bitbuf = 0u;
    r.bitcnt = 0;

    while (!last){
        unsigned int bfinal = 0u;
        unsigned int btype = 0u;
        if (inflate_bits(&r, 1, &bfinal) != 0 || inflate_bits(&r, 2, &btype) != 0){
            return -1;
        }
        last = (int)bfinal;

        if (btype == 0u){
            unsigned int len = 0u;
            unsigned int nlen = 0u;
            inflate_align_byte(&r);
            if (inflate_bits(&r, 16, &len) != 0 || inflate_bits(&r, 16, &nlen) != 0){
                return -1;
            }
            if (((len ^ 0xFFFFu) & 0xFFFFu) != (nlen & 0xFFFFu)){
                return -1;
            }
            if (out_pos + (int)len > out_cap){
                return -1;
            }
            while (len--){
                unsigned int byte = 0u;
                if (inflate_bits(&r, 8, &byte) != 0){
                    return -1;
                }
                out[out_pos++] = (unsigned char)byte;
            }
            continue;
        }

        if (btype == 1u){
            if (inflate_build_fixed(&ll, &dd) != 0){
                return -1;
            }
        } else if (btype == 2u){
            unsigned int hlit = 0u, hdist = 0u, hclen = 0u;
            unsigned char clen_lens[19];
            unsigned char ll_lens[288];
            unsigned char dd_lens[32];
            unsigned char lens[320];
            inflate_huff_t clen;
            int total, idx, sym, i;

            for (i = 0; i < 19; i++) clen_lens[i] = 0;
            for (i = 0; i < 288; i++) ll_lens[i] = 0;
            for (i = 0; i < 32; i++) dd_lens[i] = 0;

            if (inflate_bits(&r, 5, &hlit) != 0 ||
                inflate_bits(&r, 5, &hdist) != 0 ||
                inflate_bits(&r, 4, &hclen) != 0){
                return -1;
            }
            hlit += 257u;
            hdist += 1u;
            hclen += 4u;
            if (hlit > 286u || hdist > 32u){
                return -1;
            }

            for (i = 0; i < (int)hclen; i++){
                unsigned int v = 0u;
                if (inflate_bits(&r, 3, &v) != 0){
                    return -1;
                }
                clen_lens[order[i]] = (unsigned char)v;
            }
            if (inflate_huff_build(&clen, clen_lens, 19) != 0){
                return -1;
            }

            total = (int)(hlit + hdist);
            idx = 0;
            while (idx < total){
                if (inflate_huff_decode(&r, &clen, &sym) != 0){
                    return -1;
                }
                if (sym >= 0 && sym <= 15){
                    lens[idx++] = (unsigned char)sym;
                } else if (sym == 16){
                    unsigned int rep = 0u;
                    unsigned char prev;
                    if (idx == 0 || inflate_bits(&r, 2, &rep) != 0){
                        return -1;
                    }
                    prev = lens[idx - 1];
                    rep += 3u;
                    while (rep-- && idx < total){
                        lens[idx++] = prev;
                    }
                } else if (sym == 17){
                    unsigned int rep = 0u;
                    if (inflate_bits(&r, 3, &rep) != 0){
                        return -1;
                    }
                    rep += 3u;
                    while (rep-- && idx < total){
                        lens[idx++] = 0;
                    }
                } else if (sym == 18){
                    unsigned int rep = 0u;
                    if (inflate_bits(&r, 7, &rep) != 0){
                        return -1;
                    }
                    rep += 11u;
                    while (rep-- && idx < total){
                        lens[idx++] = 0;
                    }
                } else{
                    return -1;
                }
            }

            for (i = 0; i < (int)hlit; i++) ll_lens[i] = lens[i];
            for (i = 0; i < (int)hdist; i++) dd_lens[i] = lens[(int)hlit + i];
            if (inflate_huff_build(&ll, ll_lens, 288) != 0){
                return -1;
            }
            if (inflate_huff_build(&dd, dd_lens, 32) != 0){
                return -1;
            }
        } else{
            return -1;
        }

        while (1){
            int sym = 0;
            if (inflate_huff_decode(&r, &ll, &sym) != 0){
                return -1;
            }
            if (sym < 256){
                if (out_pos >= out_cap){
                    return -1;
                }
                out[out_pos++] = (unsigned char)sym;
            } else if (sym == 256){
                break;
            } else if (sym >= 257 && sym <= 285){
                unsigned int extra = 0u;
                unsigned int dist_extra_v = 0u;
                int len_idx = sym - 257;
                int dist_sym = 0;
                unsigned int len = len_base[len_idx];
                unsigned int dist;
                if (len_extra[len_idx] && inflate_bits(&r, len_extra[len_idx], &extra) != 0){
                    return -1;
                }
                len += extra;
                if (inflate_huff_decode(&r, &dd, &dist_sym) != 0 || dist_sym < 0 || dist_sym > 29){
                    return -1;
                }
                dist = dist_base[dist_sym];
                if (dist_extra[dist_sym] &&
                    inflate_bits(&r, dist_extra[dist_sym], &dist_extra_v) != 0){
                    return -1;
                }
                dist += dist_extra_v;
                if (dist == 0u || (int)dist > out_pos){
                    return -1;
                }
                if (out_pos + (int)len > out_cap){
                    return -1;
                }
                while (len--){
                    out[out_pos] = out[out_pos - (int)dist];
                    out_pos++;
                }
            } else{
                return -1;
            }
        }
    }

    return out_pos;
}

static int gzip_decompress_local(const unsigned char* in, int in_len, unsigned char* out, int out_cap){
    int pos;
    unsigned int flags;
    if (!in || in_len < 18 || !out || out_cap <= 0){
        return -1;
    }
    if (in[0] != 0x1Fu || in[1] != 0x8Bu || in[2] != 8u){
        return -1;
    }
    flags = in[3];
    pos = 10;

    if (flags & 0x04u){
        int xlen;
        if (pos + 2 > in_len){
            return -1;
        }
        xlen = (int)in[pos] | ((int)in[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08u){
        while (pos < in_len && in[pos] != 0){
            pos++;
        }
        pos++;
    }
    if (flags & 0x10u){
        while (pos < in_len && in[pos] != 0){
            pos++;
        }
        pos++;
    }
    if (flags & 0x02u){
        pos += 2;
    }
    if (pos >= in_len - 8){
        return -1;
    }

    {
        int out_n = inflate_raw_deflate_local(&in[pos], in_len - pos - 8, out, out_cap);
        if (out_n <= 0){
            return -1;
        }
        return out_n;
    }
}

static int decode_chunked_body(const unsigned char* in, int len, unsigned char* out, int out_cap){
    int i = 0;
    int o = 0;
    if (!in || len < 0 || !out || out_cap <= 0){
        return -1;
    }

    while (i < len){
        unsigned int chunk_len = 0;
        int have_hex = 0;

        while (i < len && (in[i] == '\r' || in[i] == '\n')){
            i++;
        }

        while (i < len){
            int hv = hex_value(in[i]);
            if (hv >= 0){
                if (chunk_len > 0x0FFFFFFFu){
                    return -1;
                }
                chunk_len = (chunk_len << 4) | (unsigned int)hv;
                have_hex = 1;
                i++;
                continue;
            }
            if (in[i] == ';'){
                while (i < len && in[i] != '\n'){
                    i++;
                }
                break;
            }
            if (in[i] == '\r' || in[i] == '\n'){
                break;
            }
            return -1;
        }

        if (!have_hex){
            return -1;
        }
        while (i < len && in[i] != '\n'){
            i++;
        }
        if (i < len && in[i] == '\n'){
            i++;
        }

        if (chunk_len == 0){
            return o;
        }
        if (chunk_len > (unsigned int)(len - i) || chunk_len > (unsigned int)(out_cap - o)){
            return -1;
        }
        for (unsigned int c = 0; c < chunk_len; c++){
            out[o++] = in[i + (int)c];
        }
        i += (int)chunk_len;
        if (i < len && in[i] == '\r'){
            i++;
        }
        if (i < len && in[i] == '\n'){
            i++;
        }
    }

    return o;
}

static int match_entity(const unsigned char* p, int rem, const char* ent){
    int i = 0;
    while (ent[i]){
        if (i >= rem || (unsigned char)ent[i] != p[i]){
            return 0;
        }
        i++;
    }
    return i;
}

static unsigned char ascii_lower(unsigned char c){
    if (c >= 'A' && c <= 'Z'){
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static int tag_name_is(const unsigned char* s, int len, const char* name){
    int i = 0;
    while (name[i]){
        if (i >= len){
            return 0;
        }
        if (ascii_lower(s[i]) != (unsigned char)name[i]){
            return 0;
        }
        i++;
    }
    if (i < len){
        unsigned char c = s[i];
        if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>')){
            return 0;
        }
    }
    return 1;
}

static int print_html_text(const unsigned char* html, int len){
    int in_tag = 0;
    int last_space = 1;
    int suppress_style = 0;
    int suppress_script = 0;
    int suppress_head = 0;
    int tag_start = -1;
    int visible = 0;

    for (int i = 0; i < len; i++){
        unsigned char c = html[i];

        if (in_tag){
            if (c == '>'){
                int ts = tag_start >= 0 ? tag_start : i;
                int te = i;
                while (ts < te && (html[ts] == ' ' || html[ts] == '\t' || html[ts] == '\r' || html[ts] == '\n')){
                    ts++;
                }
                int closing = 0;
                if (ts < te && html[ts] == '/'){
                    closing = 1;
                    ts++;
                }
                while (ts < te && (html[ts] == ' ' || html[ts] == '\t' || html[ts] == '\r' || html[ts] == '\n')){
                    ts++;
                }
                int nlen = te - ts;
                if (nlen > 0){
                    if (tag_name_is(&html[ts], nlen, "style")){
                        suppress_style = closing ? 0 : 1;
                    } else if (tag_name_is(&html[ts], nlen, "script")){
                        suppress_script = closing ? 0 : 1;
                    } else if (tag_name_is(&html[ts], nlen, "head")){
                        suppress_head = closing ? 0 : 1;
                    }
                }
                in_tag = 0;
                tag_start = -1;
                if (!last_space){
                    qos_putc(' ');
                    last_space = 1;
                }
            }
            continue;
        }

        if (c == '<'){
            in_tag = 1;
            tag_start = i + 1;
            continue;
        }

        if (suppress_style || suppress_script || suppress_head){
            continue;
        }

        if (c == '&'){
            int rem = len - i;
            int m = 0;
            if ((m = match_entity(&html[i], rem, "&nbsp;")) > 0 ||
                (m = match_entity(&html[i], rem, "&#160;")) > 0){
                c = ' ';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&amp;")) > 0){
                c = '&';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&lt;")) > 0){
                c = '<';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&gt;")) > 0){
                c = '>';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&quot;")) > 0){
                c = '"';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&#39;")) > 0){
                c = '\'';
                i += (m - 1);
            }
        }

        if (c == '\r'){
            continue;
        }

        if (c == '\n' || c == '\t' || c == ' '){
            if (!last_space){
                qos_putc(' ');
                last_space = 1;
            }
            continue;
        }

        if (c < 32u || c > 126u){
            continue;
        }

        qos_putc((char)c);
        visible++;
        last_space = 0;
    }
    qos_puts("\n");
    return visible;
}

static int http_fetch_raw(const char* host, const char* path, unsigned short port, unsigned char* resp, int resp_cap){
    unsigned char ip[4];
    qos_sockaddr_in_t sa;
    char req[REQ_CAP];
    int rq = 0;
    unsigned long t_total_start = qos_get_ticks();
    unsigned long t_stage_start = t_total_start;
    unsigned long dns_ms = 0;
    unsigned long connect_ms = 0;
    unsigned long send_ms = 0;
    unsigned long first_chunk_ms = 0;
    unsigned long recv_ms = 0;
    unsigned int recv_chunks = 0u;

    int dns_rc = qos_dns_resolve_a_socket(host, g_dns_server, ip, DNS_TIMEOUT_MS);
    dns_ms = qos_get_ticks() - t_stage_start;
    if (dns_rc != 0){
        qos_puts("DNS resolve failed.\n");
        print_fetch_timing(dns_ms, connect_ms, send_ms, first_chunk_ms, recv_ms, recv_chunks, qos_get_ticks() - t_total_start);
        return -1;
    }

    qos_puts("Resolved ");
    qos_puts(host);
    qos_puts(" -> ");
    print_ip4(ip);
    qos_puts("\n");

    int fd = qos_socket(QOS_AF_INET, QOS_SOCK_STREAM, 0);
    if (fd < 0){
        qos_puts("socket() failed.\n");
        print_fetch_timing(dns_ms, connect_ms, send_ms, first_chunk_ms, recv_ms, recv_chunks, qos_get_ticks() - t_total_start);
        return -1;
    }

    (void)qos_socket_set_nonblocking(fd, 0);
    (void)qos_socket_set_recv_timeout(fd, RECV_TIMEOUT_MS);

    sa.family = QOS_AF_INET;
    sa.port = port;
    sa.addr[0] = ip[0];
    sa.addr[1] = ip[1];
    sa.addr[2] = ip[2];
    sa.addr[3] = ip[3];
    for (int i = 0; i < (int)sizeof(sa.reserved); i++){
        sa.reserved[i] = 0;
    }

    t_stage_start = qos_get_ticks();
    if (qos_connect(fd, &sa, (unsigned int)sizeof(sa)) != 0){
        connect_ms = qos_get_ticks() - t_stage_start;
        qos_puts("connect() failed.\n");
        (void)qos_close(fd);
        print_fetch_timing(dns_ms, connect_ms, send_ms, first_chunk_ms, recv_ms, recv_chunks, qos_get_ticks() - t_total_start);
        return -1;
    }
    connect_ms = qos_get_ticks() - t_stage_start;

    if (append_str(req, REQ_CAP, &rq, "GET ") != 0 ||
        append_str(req, REQ_CAP, &rq, (path && *path) ? path : "/") != 0 ||
        append_str(req, REQ_CAP, &rq, " HTTP/1.1\r\nHost: ") != 0 ||
        append_str(req, REQ_CAP, &rq, host) != 0 ||
        append_str(req, REQ_CAP, &rq,
                   "\r\nUser-Agent: QOS-WebBrowser/0.1"
                   "\r\nAccept: text/html,text/plain,*/*;q=0.8") != 0 ||
        append_str(req, REQ_CAP, &rq,
                   g_accept_gzip ? "\r\nAccept-Encoding: gzip, identity" :
                                   "\r\nAccept-Encoding: identity") != 0 ||
        append_str(req, REQ_CAP, &rq, "\r\nConnection: close\r\n\r\n") != 0){
        qos_puts("Request build failed.\n");
        (void)qos_close(fd);
        return -1;
    }

    t_stage_start = qos_get_ticks();
    int send_rc = qos_send(fd, req, (unsigned int)rq, 0);
    send_ms = qos_get_ticks() - t_stage_start;
    if (send_rc < 0){
        qos_puts("send() failed rc=");
        print_int(send_rc);
        qos_puts("\n");
        (void)qos_close(fd);
        print_fetch_timing(dns_ms, connect_ms, send_ms, first_chunk_ms, recv_ms, recv_chunks, qos_get_ticks() - t_total_start);
        return -1;
    }

    int total = 0;
    unsigned int stall_rounds = 0u;
    t_stage_start = qos_get_ticks();
    while (total < resp_cap){
        int n = qos_recv(fd, &resp[total], (unsigned int)(resp_cap - total), QOS_SOCK_TIMEOUT_USE_SOCKET);
        if (n == QOS_SOCK_ERR_AGAIN){
            continue;
        }
        if (n <= 0){
            int completion = http_response_completion_state_local(resp, total);
            if (n == 0 && completion == 0 && stall_rounds < 1u){
                stall_rounds++;
                continue;
            }
            break;
        }
        stall_rounds = 0u;
        if (recv_chunks == 0u){
            first_chunk_ms = qos_get_ticks() - t_stage_start;
        }
        recv_chunks++;
        total += n;
        if (recv_chunks <= 2u || (recv_chunks & 7u) == 0u || total >= (resp_cap - 2048)){
            if (http_response_completion_state_local(resp, total) == 1){
                break;
            }
        }
    }
    recv_ms = qos_get_ticks() - t_stage_start;

    (void)qos_close(fd);
    print_fetch_timing(dns_ms, connect_ms, send_ms, first_chunk_ms, recv_ms, recv_chunks, qos_get_ticks() - t_total_start);
    return total;
}

static int http_fetch_follow_redirects(int use_https,
                                       const char* host_in,
                                       const char* path_in,
                                       unsigned char* resp,
                                       int resp_cap){
    char host[HOST_CAP];
    char path[PATH_CAP];
    char location[URL_CAP];
    int redirects = 0;
    int n = 0;

    if (copy_cstr(host, sizeof(host), host_in) != 0 ||
        copy_cstr(path, sizeof(path), path_in) != 0){
        return -1;
    }

    while (1){
        unsigned short port = use_https ? 443u : 80u;
        int status;

        n = http_fetch_raw(host, path, port, resp, resp_cap);
        if (n <= 0){
            return n;
        }

        status = parse_http_status_code(resp, n);
        if ((status != 301 && status != 302 && status != 303 && status != 307 && status != 308) ||
            redirects >= MAX_REDIRECTS){
            return n;
        }

        if (extract_header_value(resp, n, "Location", location, sizeof(location)) != 0){
            return n;
        }

        qos_puts("Redirect -> ");
        qos_puts(location);
        qos_puts("\n");

        if (resolve_redirect_target(location, use_https, host, path,
                                    &use_https, host, sizeof(host), path, sizeof(path)) != 0){
            return n;
        }

        redirects++;
    }
}

static void cmd_help(void){
    qos_puts("Commands:\n");
    qos_puts(" help\n");
    qos_puts(" open <url|host> [path]\n");
    qos_puts(" gzip on|off|status\n");
    qos_puts(" exit\n");
}

static void cmd_gzip(const char* arg){
    while (arg && *arg == ' '){
        arg++;
    }
    if (!arg || !*arg || str_eq(arg, "status")){
        qos_puts("gzip accept=");
        qos_puts(g_accept_gzip ? "on" : "off");
        qos_puts("\n");
        return;
    }
    if (str_eq(arg, "on")){
        g_accept_gzip = 1;
        qos_puts("gzip accept=on\n");
        return;
    }
    if (str_eq(arg, "off")){
        g_accept_gzip = 0;
        qos_puts("gzip accept=off\n");
        return;
    }
    qos_puts("Usage: gzip on|off|status\n");
}

static void cmd_open(char* host, const char* path){
    static unsigned char resp[RESP_CAP];
    static unsigned char work[RESP_CAP];
    char req_host[HOST_CAP];
    char req_path[PATH_CAP];
    char target[URL_CAP];
    char header_value[64];
    int use_https = 0;
    int n;
    unsigned long decode_chunked_ms = 0;
    unsigned long decode_gzip_ms = 0;
    unsigned long render_ms = 0;

    if (!host || !*host){
        qos_puts("Usage: open <url|host> [path]\n");
        return;
    }

    if (copy_cstr(target, sizeof(target), host) != 0){
        qos_puts("Target too long.\n");
        return;
    }
    if (path && *path){
        int tlen = 0;
        while (target[tlen]){
            tlen++;
        }
        if (tlen > 0 && target[tlen - 1] != '/' && path[0] != '/'){
            if (append_char(target, sizeof(target), &tlen, '/') != 0){
                qos_puts("Target too long.\n");
                return;
            }
        }
        if (append_str(target, sizeof(target), &tlen, path) != 0){
            qos_puts("Target too long.\n");
            return;
        }
    }

    if (parse_url_target(target, 0, &use_https, req_host, sizeof(req_host), req_path, sizeof(req_path)) != 0){
        qos_puts("Usage: open <url|host> [path]\n");
        return;
    }

    qos_puts("Fetching ");
    qos_puts(use_https ? "https://" : "http://");
    qos_puts(req_host);
    qos_puts(req_path);
    qos_puts("\n");

    n = http_fetch_follow_redirects(use_https, req_host, req_path, resp, (int)sizeof(resp));
    if (n <= 0){
        qos_puts("Fetch failed or empty.\n");
        return;
    }

    qos_puts("Received bytes=");
    print_uint((unsigned int)n);
    qos_puts("\n");

    int status = parse_http_status_code(resp, n);
    if (status > 0){
        qos_puts("HTTP status=");
        print_uint((unsigned int)status);
        qos_puts("\n");
    }
    if (n >= ((int)sizeof(resp) - 2)){
        qos_puts("Response reached browser buffer limit; output may be truncated.\n");
    }

    int body = find_http_body(resp, n);
    if (body <= 0 || body >= n){
        qos_puts("No HTTP body found in response.\n");
        return;
    }

    const unsigned char* body_ptr = &resp[body];
    int body_len = n - body;
    if (extract_header_value(resp, n, "Transfer-Encoding", header_value, sizeof(header_value)) == 0 &&
        str_contains_ci(header_value, "chunked")){
        unsigned long t_decode = qos_get_ticks();
        int decoded = decode_chunked_body(body_ptr, body_len, work, (int)sizeof(work));
        decode_chunked_ms = qos_get_ticks() - t_decode;
        if (decoded > 0){
            body_ptr = work;
            body_len = decoded;
        } else{
            qos_puts("Chunked response decode failed; showing raw body.\n");
        }
    }

    if (extract_header_value(resp, n, "Content-Encoding", header_value, sizeof(header_value)) == 0){
        if (str_contains_ci(header_value, "gzip")){
            unsigned char* out_buf = (body_ptr == work) ? resp : work;
            unsigned long t_decode = qos_get_ticks();
            int decoded = gzip_decompress_local(body_ptr, body_len, out_buf, RESP_CAP);
            decode_gzip_ms = qos_get_ticks() - t_decode;
            if (decoded > 0){
                body_ptr = out_buf;
                body_len = decoded;
            } else{
                qos_puts("Gzip decode failed; showing raw body.\n");
            }
        } else if (!str_contains_ci(header_value, "identity")){
            qos_puts("Unsupported Content-Encoding=");
            qos_puts(header_value);
            qos_puts(" (showing raw body)\n");
        }
    }

    qos_puts("\n");
    unsigned long t_render = qos_get_ticks();
    int visible = print_html_text(body_ptr, body_len);
    render_ms = qos_get_ticks() - t_render;
    qos_puts("Decode/render timing ms: chunked=");
    print_uint((unsigned int)decode_chunked_ms);
    qos_puts(" gzip=");
    print_uint((unsigned int)decode_gzip_ms);
    qos_puts(" render=");
    print_uint((unsigned int)render_ms);
    qos_puts("\n");
    if (visible == 0){
        qos_puts("No visible text in received body. It may be mostly head/script/style content or unsupported markup.\n");
    }
}

static void print_prompt(void){
    qos_puts("\nWEB> ");
}

static void execute_line(void){
    if (g_input_len <= 0){
        return;
    }
    g_input[g_input_len] = 0;

    if (str_eq(g_input, "help")){
        cmd_help();
        return;
    }
    if (str_eq(g_input, "exit")){
        (void)qos_tty_release();
        qos_exit(0);
    }

    if (str_starts_with(g_input, "gzip")){
        char* p = g_input + 4;
        cmd_gzip(p);
        return;
    }

    if (str_starts_with(g_input, "open ")){
        char* p = g_input + 5;
        char* host;
        char* path = 0;

        while (*p == ' '){
            p++;
        }
        host = p;

        while (*p && *p != ' '){
            p++;
        }
        if (*p){
            *p++ = 0;
            while (*p == ' '){
                p++;
            }
            if (*p){
                path = p;
            }
        }

        if (!*host){
            qos_puts("Usage: open <url|host> [path]\n");
            return;
        }
        cmd_open(host, path);
        return;
    }

    qos_puts("Unknown command. Type 'help'.\n");
}

void program_main(void){
    qos_puts("WebBrowser (text mode) ready.\n");
    qos_puts("Type 'help' for commands.\n");
    print_prompt();

    while (1){
        int ch = qos_try_getc();
        if (ch < 0){
            continue;
        }

        if (ch == '\r' || ch == '\n'){
            qos_puts("\n");
            execute_line();
            g_input_len = 0;
            g_input[0] = 0;
            print_prompt();
            continue;
        }

        if (ch == 127 || ch == '\b'){
            if (g_input_len > 0){
                g_input_len--;
                g_input[g_input_len] = 0;
                qos_puts("\b \b");
            }
            continue;
        }

        if (g_input_len < (INPUT_CAP - 1)){
            g_input[g_input_len++] = (char)ch;
            qos_putc((char)ch);
        }
    }
}
