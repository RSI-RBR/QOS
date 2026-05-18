#include "sandbox_file.h"
#include "fat32.h"
#include "memory.h"
#include "spinlock.h"

#define SANDBOX_FILE_ENTRIES 24u
#define SANDBOX_FILE_COMPONENT_MAX 64u
#define SANDBOX_FILE_INITIAL_CAP 1024u

typedef struct sandbox_file_entry {
    unsigned int used;
    char sandbox83[11];
    char path[SANDBOX_FILE_PATH_MAX];
    unsigned char* data;
    unsigned int size;
    unsigned int cap;
    unsigned int fat_flushed_size;
} sandbox_file_entry_t;

static sandbox_file_entry_t g_files[SANDBOX_FILE_ENTRIES];
static spinlock_t g_files_lock;
static unsigned int g_files_inited = 0u;

static int sandbox83_eq(const char a[11], const char b[11]){
    if (!a || !b){
        return 0;
    }
    for (unsigned int i = 0; i < 11u; i++){
        if (a[i] != b[i]){
            return 0;
        }
    }
    return 1;
}

static int path_eq(const char* a, const char* b){
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

static void copy_path(char dst[SANDBOX_FILE_PATH_MAX], const char* src){
    unsigned int i = 0u;
    if (!dst || !src){
        return;
    }
    for (; i + 1u < SANDBOX_FILE_PATH_MAX && src[i]; i++){
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static int path_valid(const char* path){
    unsigned int i = 0u;
    unsigned int seg_len = 0u;
    char seg0 = 0;
    char seg1 = 0;

    if (!path || !path[0]){
        return 0;
    }
    if (path[0] == '/' || path[0] == '\\'){
        return 0;
    }

    while (1){
        char c = path[i];
        if (c == 0){
            if (seg_len == 0u){
                return 0;
            }
            if (seg_len == 1u && seg0 == '.'){
                return 0;
            }
            if (seg_len == 2u && seg0 == '.' && seg1 == '.'){
                return 0;
            }
            return 1;
        }

        if (i + 1u >= SANDBOX_FILE_PATH_MAX){
            return 0;
        }
        if ((unsigned char)c < 32u || (unsigned char)c == 127u || c == ':'){
            return 0;
        }

        if (c == '/' || c == '\\'){
            if (seg_len == 0u){
                return 0;
            }
            if (seg_len == 1u && seg0 == '.'){
                return 0;
            }
            if (seg_len == 2u && seg0 == '.' && seg1 == '.'){
                return 0;
            }
            seg_len = 0u;
            seg0 = 0;
            seg1 = 0;
            i++;
            continue;
        }

        if (seg_len == 0u){
            seg0 = c;
        } else if (seg_len == 1u){
            seg1 = c;
        }
        seg_len++;
        if (seg_len > SANDBOX_FILE_COMPONENT_MAX){
            return 0;
        }
        i++;
    }
}

static sandbox_file_entry_t* find_entry_locked(const char sandbox83[11], const char* path){
    for (unsigned int i = 0; i < SANDBOX_FILE_ENTRIES; i++){
        sandbox_file_entry_t* e = &g_files[i];
        if (!e->used){
            continue;
        }
        if (sandbox83_eq(e->sandbox83, sandbox83) && path_eq(e->path, path)){
            return e;
        }
    }
    return 0;
}

static sandbox_file_entry_t* alloc_entry_locked(void){
    for (unsigned int i = 0; i < SANDBOX_FILE_ENTRIES; i++){
        if (!g_files[i].used){
            g_files[i].used = 1u;
            g_files[i].size = 0u;
            g_files[i].cap = 0u;
            g_files[i].fat_flushed_size = 0u;
            g_files[i].data = 0;
            for (unsigned int k = 0; k < 11u; k++){
                g_files[i].sandbox83[k] = ' ';
            }
            g_files[i].path[0] = 0;
            return &g_files[i];
        }
    }
    return 0;
}

void sandbox_file_init(void){
    if (g_files_inited){
        return;
    }
    spinlock_init(&g_files_lock);
    for (unsigned int i = 0; i < SANDBOX_FILE_ENTRIES; i++){
        g_files[i].used = 0u;
        g_files[i].data = 0;
        g_files[i].size = 0u;
        g_files[i].cap = 0u;
        g_files[i].fat_flushed_size = 0u;
        g_files[i].path[0] = 0;
    }
    g_files_inited = 1u;
}

int sandbox_file_append(const char sandbox83[11],
                        const char* relative_path,
                        const unsigned char* data,
                        unsigned int len){
    sandbox_file_entry_t* e;
    unsigned int new_size;
    unsigned int new_cap;
    unsigned char* new_buf;

    if (!g_files_inited){
        sandbox_file_init();
    }
    if (!sandbox83 || !relative_path || !path_valid(relative_path)){
        return -1;
    }
    if (len == 0u){
        return 0;
    }
    if (!data || len > SANDBOX_FILE_WRITE_MAX){
        return -1;
    }

    spin_lock(&g_files_lock);
    e = find_entry_locked(sandbox83, relative_path);
    if (!e){
        e = alloc_entry_locked();
        if (!e){
            spin_unlock(&g_files_lock);
            return -1;
        }
        for (unsigned int k = 0; k < 11u; k++){
            e->sandbox83[k] = sandbox83[k];
        }
        copy_path(e->path, relative_path);
    }

    if (e->size > SANDBOX_FILE_MAX_BYTES || len > SANDBOX_FILE_MAX_BYTES - e->size){
        spin_unlock(&g_files_lock);
        return -1;
    }
    new_size = e->size + len;
    if (new_size > SANDBOX_FILE_MAX_BYTES){
        spin_unlock(&g_files_lock);
        return -1;
    }

    if (e->cap < new_size){
        new_cap = e->cap ? e->cap : SANDBOX_FILE_INITIAL_CAP;
        while (new_cap < new_size){
            if (new_cap > SANDBOX_FILE_MAX_BYTES / 2u){
                new_cap = SANDBOX_FILE_MAX_BYTES;
                break;
            }
            new_cap <<= 1u;
        }
        if (new_cap < new_size || new_cap > SANDBOX_FILE_MAX_BYTES){
            spin_unlock(&g_files_lock);
            return -1;
        }
        new_buf = (unsigned char*)kmalloc(new_cap);
        if (!new_buf){
            spin_unlock(&g_files_lock);
            return -1;
        }
        for (unsigned int i = 0u; i < e->size; i++){
            new_buf[i] = e->data[i];
        }
        if (e->data && e->cap){
            kfree_secure(e->data, e->cap);
        }
        e->data = new_buf;
        e->cap = new_cap;
    }

    for (unsigned int i = 0u; i < len; i++){
        e->data[e->size + i] = data[i];
    }
    e->size = new_size;
    spin_unlock(&g_files_lock);
    return (int)len;
}

int sandbox_file_read(const char sandbox83[11],
                      const char* relative_path,
                      unsigned char* out,
                      unsigned int out_cap){
    return sandbox_file_read_at(sandbox83, relative_path, 0u, out, out_cap);
}

int sandbox_file_read_at(const char sandbox83[11],
                         const char* relative_path,
                         unsigned int offset,
                         unsigned char* out,
                         unsigned int out_cap){
    sandbox_file_entry_t* e;
    unsigned int n;

    if (!g_files_inited){
        sandbox_file_init();
    }
    if (!sandbox83 || !relative_path || !out || out_cap == 0u || !path_valid(relative_path)){
        return -1;
    }

    spin_lock(&g_files_lock);
    e = find_entry_locked(sandbox83, relative_path);
    if (!e || !e->data){
        spin_unlock(&g_files_lock);
        return -1;
    }
    if (offset >= e->size){
        spin_unlock(&g_files_lock);
        return 0;
    }

    n = ((e->size - offset) < out_cap) ? (e->size - offset) : out_cap;
    for (unsigned int i = 0u; i < n; i++){
        out[i] = e->data[offset + i];
    }
    spin_unlock(&g_files_lock);
    return (int)n;
}

int sandbox_file_size(const char sandbox83[11], const char* relative_path){
    sandbox_file_entry_t* e;
    if (!g_files_inited){
        sandbox_file_init();
    }
    if (!sandbox83 || !relative_path || !path_valid(relative_path)){
        return -1;
    }
    spin_lock(&g_files_lock);
    e = find_entry_locked(sandbox83, relative_path);
    if (!e){
        spin_unlock(&g_files_lock);
        return -1;
    }
    int n = (int)e->size;
    spin_unlock(&g_files_lock);
    return n;
}

int sandbox_file_clear(const char sandbox83[11], const char* relative_path){
    sandbox_file_entry_t* e;
    if (!g_files_inited){
        sandbox_file_init();
    }
    if (!sandbox83 || !relative_path || !path_valid(relative_path)){
        return -1;
    }
    spin_lock(&g_files_lock);
    e = find_entry_locked(sandbox83, relative_path);
    if (!e){
        spin_unlock(&g_files_lock);
        return -1;
    }
    if (e->data && e->cap){
        for (unsigned int i = 0u; i < e->cap; i++){
            e->data[i] = 0u;
        }
    }
    e->size = 0u;
    e->fat_flushed_size = 0u;
    spin_unlock(&g_files_lock);
    return 0;
}

int sandbox_file_flush_to_fat(const char sandbox83[11], const char* relative_path){
    sandbox_file_entry_t* e;
    unsigned char* copy = 0;
    unsigned int offset = 0u;
    unsigned int len = 0u;
    int rc;

    if (!g_files_inited){
        sandbox_file_init();
    }
    if (!sandbox83 || !relative_path || !path_valid(relative_path)){
        return -1;
    }

    spin_lock(&g_files_lock);
    e = find_entry_locked(sandbox83, relative_path);
    if (!e){
        spin_unlock(&g_files_lock);
        return -1;
    }
    if (e->fat_flushed_size > e->size){
        e->fat_flushed_size = e->size;
    }
    offset = e->fat_flushed_size;
    len = e->size - offset;
    if (len > 0u){
        copy = (unsigned char*)kmalloc(len);
        if (!copy){
            spin_unlock(&g_files_lock);
            return -1;
        }
        for (unsigned int i = 0u; i < len; i++){
            copy[i] = e->data[offset + i];
        }
    }
    spin_unlock(&g_files_lock);

    if (len == 0u){
        return 0;
    }

    rc = fat32_append_file_in_dir_path_existing(sandbox83, relative_path, copy, len);
    if (copy){
        kfree_secure(copy, len);
    }

    if (rc == 0){
        spin_lock(&g_files_lock);
        e = find_entry_locked(sandbox83, relative_path);
        if (e && e->fat_flushed_size == offset){
            unsigned int flushed_end = offset + len;
            if (flushed_end <= e->size){
                unsigned int remaining = e->size - flushed_end;
                for (unsigned int i = 0u; i < remaining; i++){
                    e->data[i] = e->data[flushed_end + i];
                }
                for (unsigned int i = remaining; i < e->size; i++){
                    e->data[i] = 0u;
                }
                e->size = remaining;
                e->fat_flushed_size = 0u;
            } else{
                e->fat_flushed_size = e->size;
            }
        }
        spin_unlock(&g_files_lock);
    }
    return rc;
}

int sandbox_file_flush_to_fat_replace(const char sandbox83[11], const char* relative_path){
    sandbox_file_entry_t* e;
    unsigned char* copy = 0;
    unsigned int len = 0u;
    int rc;

    if (!g_files_inited){
        sandbox_file_init();
    }
    if (!sandbox83 || !relative_path || !path_valid(relative_path)){
        return -1;
    }

    spin_lock(&g_files_lock);
    e = find_entry_locked(sandbox83, relative_path);
    if (!e){
        spin_unlock(&g_files_lock);
        return -1;
    }
    len = e->size;
    if (len > 0u){
        copy = (unsigned char*)kmalloc(len);
        if (!copy){
            spin_unlock(&g_files_lock);
            return -1;
        }
        for (unsigned int i = 0u; i < len; i++){
            copy[i] = e->data[i];
        }
    }
    spin_unlock(&g_files_lock);

    rc = fat32_write_file_in_dir_path_existing(sandbox83, relative_path, copy, len);
    if (copy){
        kfree_secure(copy, len);
    }
    return rc;
}
