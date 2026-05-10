#include "v3d.h"
#include "cache.h"
#include "framebuffer.h"
#include "mailbox.h"
#include "spinlock.h"

#define V3D_MMIO_BASE        0x3FC00000UL
#define V3D_CLOCK_ID         5u
#define V3D_CLOCK_TARGET_HZ  250000000u

#define V3D_IDENT0           0x00000UL
#define V3D_IDENT1           0x00004UL
#define V3D_IDENT2           0x00008UL
#define V3D_SCRATCH          0x00010UL
#define V3D_INTCTL           0x00030UL
#define V3D_INTDIS           0x00038UL
#define V3D_CT0CS            0x00100UL
#define V3D_CT1CS            0x00104UL
#define V3D_CT0EA            0x00108UL
#define V3D_CT1EA            0x0010CUL
#define V3D_CT0CA            0x00110UL
#define V3D_CT1CA            0x00114UL
#define V3D_ERRSTAT          0x00F20UL

#define V3D_CTRSTA           (1u << 15)
#define V3D_CTRUN            (1u << 5)
#define V3D_CTERR            (1u << 3)

#define V3D_EXPECTED_IDENT0  ((2u << 24) | ('D' << 16) | ('3' << 8) | 'V')
#define V3D_SCRATCH_TEST     0x51563344u
#define V3D_BUS_UNCACHED_BASE 0xC0000000UL
#define V3D_NOOP_CL_SIZE      64u
#define V3D_NOOP_TIMEOUT      1000000u
#define V3D_CLEAR_CL_SIZE     16384u
#define V3D_TILE_SIZE         64u
#define V3D_CL_HALT           0u
#define V3D_CL_STORE_RESOLVED 24u
#define V3D_CL_STORE_EOF      25u
#define V3D_CL_RENDER_CONFIG  113u
#define V3D_CL_CLEAR_COLORS   114u
#define V3D_CL_TILE_COORDS    115u
#define V3D_RENDER_RGBA8888_LINEAR (1u << 2)

static qos_v3d_status_t g_v3d_status;
static unsigned int g_v3d_probe_count = 0u;
static unsigned int g_v3d_fail_count = 0u;
static unsigned int g_v3d_noop_count = 0u;
static unsigned int g_v3d_clear_count = 0u;
static unsigned char g_v3d_noop_cl[V3D_NOOP_CL_SIZE] __attribute__((aligned(64)));
static unsigned char g_v3d_clear_cl[V3D_CLEAR_CL_SIZE] __attribute__((aligned(64)));
static spinlock_t g_v3d_lock;
static int g_v3d_lock_ready = 0;

static unsigned int v3d_read(unsigned long reg){
    return *(volatile unsigned int*)(V3D_MMIO_BASE + reg);
}

static void v3d_write(unsigned long reg, unsigned int value){
    *(volatile unsigned int*)(V3D_MMIO_BASE + reg) = value;
}

static void v3d_barrier(void){
    asm volatile("dsb sy" ::: "memory");
}

static int v3d_ident_is_valid(unsigned int ident0){
    return ident0 == V3D_EXPECTED_IDENT0;
}

static void v3d_lock_init_once(void){
    if (!g_v3d_lock_ready){
        spinlock_init(&g_v3d_lock);
        g_v3d_lock_ready = 1;
    }
}

static unsigned int v3d_bus_address(const void* p){
    unsigned long addr = (unsigned long)p;
    return (unsigned int)((addr & 0x3FFFFFFFUL) | V3D_BUS_UNCACHED_BASE);
}

static void v3d_emit_u8(unsigned char** pp, unsigned char* end, unsigned int v){
    if (*pp < end){
        *(*pp)++ = (unsigned char)v;
    }
}

static void v3d_emit_u16(unsigned char** pp, unsigned char* end, unsigned int v){
    v3d_emit_u8(pp, end, v);
    v3d_emit_u8(pp, end, v >> 8);
}

static void v3d_emit_u24(unsigned char** pp, unsigned char* end, unsigned int v){
    v3d_emit_u8(pp, end, v);
    v3d_emit_u8(pp, end, v >> 8);
    v3d_emit_u8(pp, end, v >> 16);
}

static void v3d_emit_u32(unsigned char** pp, unsigned char* end, unsigned int v){
    v3d_emit_u8(pp, end, v);
    v3d_emit_u8(pp, end, v >> 8);
    v3d_emit_u8(pp, end, v >> 16);
    v3d_emit_u8(pp, end, v >> 24);
}

static void v3d_read_register_snapshot(qos_v3d_status_t* st){
    st->ident0 = v3d_read(V3D_IDENT0);
    st->ident1 = v3d_read(V3D_IDENT1);
    st->ident2 = v3d_read(V3D_IDENT2);
    st->ct0cs = v3d_read(V3D_CT0CS);
    st->ct1cs = v3d_read(V3D_CT1CS);
    st->ct0ca = v3d_read(V3D_CT0CA);
    st->ct1ca = v3d_read(V3D_CT1CA);
    st->ct0ea = v3d_read(V3D_CT0EA);
    st->ct1ea = v3d_read(V3D_CT1EA);
    st->intctl = v3d_read(V3D_INTCTL);
    st->errstat = v3d_read(V3D_ERRSTAT);
}

static int v3d_wait_thread_stopped(unsigned int cs_reg){
    for (unsigned int i = 0u; i < V3D_NOOP_TIMEOUT; i++){
        unsigned int cs = v3d_read(cs_reg);
        if ((cs & V3D_CTERR) != 0u){
            return QOS_V3D_ERR_CONTROL;
        }
        if ((cs & V3D_CTRUN) == 0u){
            return 0;
        }
        if ((i & 0x3FFu) == 0u){
            asm volatile("yield" ::: "memory");
        }
    }
    return QOS_V3D_ERR_TIMEOUT;
}

static int v3d_update_clock(qos_v3d_status_t* st, int allow_set){
    unsigned int hz = 0u;

    st->flags &= ~QOS_V3D_FLAG_CLOCK_OK;
    if (mailbox_get_clock_rate(V3D_CLOCK_ID, &hz) == 0 && hz != 0u){
        st->clock_hz = hz;
        st->flags |= QOS_V3D_FLAG_CLOCK_OK;
        return 0;
    }

    if (allow_set &&
        mailbox_set_clock_rate(V3D_CLOCK_ID, V3D_CLOCK_TARGET_HZ) == 0 &&
        mailbox_get_clock_rate(V3D_CLOCK_ID, &hz) == 0 &&
        hz != 0u){
        st->clock_hz = hz;
        st->flags |= QOS_V3D_FLAG_CLOCK_OK;
        return 0;
    }

    st->clock_hz = 0u;
    return -1;
}

static void v3d_store_status(const qos_v3d_status_t* st){
    g_v3d_status = *st;
}

int v3d_probe(qos_v3d_status_t* out){
    qos_v3d_status_t st;
    int clock_ok;
    st.flags = QOS_V3D_FLAG_PROBED;
    st.ident0 = 0u;
    st.ident1 = 0u;
    st.ident2 = 0u;
    st.scratch_before = 0u;
    st.scratch_after = 0u;
    st.ct0cs = 0u;
    st.ct1cs = 0u;
    st.ct0ca = 0u;
    st.ct1ca = 0u;
    st.ct0ea = 0u;
    st.ct1ea = 0u;
    st.intctl = 0u;
    st.errstat = 0u;
    st.clock_hz = 0u;
    st.probe_count = ++g_v3d_probe_count;
    st.fail_count = g_v3d_fail_count;
    st.noop_count = g_v3d_noop_count;
    st.clear_count = g_v3d_clear_count;
    st.last_job_thread = g_v3d_status.last_job_thread;
    st.last_job_start_bus = g_v3d_status.last_job_start_bus;
    st.last_job_end_bus = g_v3d_status.last_job_end_bus;
    st.last_clear_color = g_v3d_status.last_clear_color;
    st.last_clear_page = g_v3d_status.last_clear_page;
    st.last_clear_tiles = g_v3d_status.last_clear_tiles;
    st.last_error = 0;

    clock_ok = (v3d_update_clock(&st, 1) == 0);

    v3d_barrier();
    v3d_read_register_snapshot(&st);
    if (!v3d_ident_is_valid(st.ident0) && mailbox_set_qpu_enabled(1u) == 0){
        st.flags |= QOS_V3D_FLAG_QPU_OK;
        v3d_barrier();
        v3d_read_register_snapshot(&st);
    }

    if (v3d_ident_is_valid(st.ident0)){
        st.flags |= QOS_V3D_FLAG_PRESENT | QOS_V3D_FLAG_IDENT_OK;
        /*
         * Keep step 1 read-mostly. The scratch register is the only write we
         * do, and only after the expected identity value proves the block is
         * reachable.
         */
        st.scratch_before = v3d_read(V3D_SCRATCH);
        v3d_write(V3D_SCRATCH, V3D_SCRATCH_TEST);
        v3d_barrier();
        st.scratch_after = v3d_read(V3D_SCRATCH);
        v3d_write(V3D_SCRATCH, st.scratch_before);
        v3d_barrier();

        if (st.scratch_after == V3D_SCRATCH_TEST){
            st.flags |= QOS_V3D_FLAG_SCRATCH_OK;
        } else{
            st.last_error = QOS_V3D_ERR_SCRATCH;
        }

        /* Leave interrupts disabled until we submit real jobs. */
        v3d_write(V3D_INTDIS, 0xFFFFFFFFu);
        v3d_barrier();
        st.intctl = v3d_read(V3D_INTCTL);
    } else{
        st.last_error = clock_ok ? QOS_V3D_ERR_IDENT : QOS_V3D_ERR_CLOCK;
    }

    if (st.last_error != 0){
        st.fail_count = ++g_v3d_fail_count;
    } else{
        st.fail_count = g_v3d_fail_count;
    }

    v3d_store_status(&st);
    if (out){
        *out = st;
    }
    return st.last_error;
}

int v3d_get_status(qos_v3d_status_t* out){
    qos_v3d_status_t st = g_v3d_status;

    if ((st.flags & QOS_V3D_FLAG_PROBED) == 0u){
        st.probe_count = g_v3d_probe_count;
        st.fail_count = g_v3d_fail_count;
        st.noop_count = g_v3d_noop_count;
        st.clear_count = g_v3d_clear_count;
        st.last_error = QOS_V3D_ERR_NOT_PROBED;
        if (out){
            *out = st;
        }
        return st.last_error;
    }

    (void)v3d_update_clock(&st, 0);
    if (st.flags & QOS_V3D_FLAG_PRESENT){
        v3d_read_register_snapshot(&st);
    }
    st.probe_count = g_v3d_probe_count;
    st.fail_count = g_v3d_fail_count;
    st.noop_count = g_v3d_noop_count;
    st.clear_count = g_v3d_clear_count;
    v3d_store_status(&st);
    if (out){
        *out = st;
    }
    return st.last_error;
}

int v3d_submit_noop(unsigned int thread, qos_v3d_status_t* out){
    qos_v3d_status_t st;
    unsigned int cs_reg;
    unsigned int ca_reg;
    unsigned int ea_reg;
    unsigned int start_bus;
    unsigned int end_bus;
    int rc = 0;

    if (thread > 1u){
        thread = 1u;
    }

    if ((g_v3d_status.flags & QOS_V3D_FLAG_SCRATCH_OK) == 0u){
        rc = v3d_probe(&st);
        if (rc != 0){
            if (out){
                *out = st;
            }
            return rc;
        }
    }

    v3d_lock_init_once();
    spin_lock(&g_v3d_lock);

    cs_reg = thread ? V3D_CT1CS : V3D_CT0CS;
    ca_reg = thread ? V3D_CT1CA : V3D_CT0CA;
    ea_reg = thread ? V3D_CT1EA : V3D_CT0EA;

    /*
     * Control-list opcode 0 is HALT. This intentionally does no rendering;
     * it only proves that the V3D command executor can fetch a kernel-owned
     * control list through the VC bus and stop cleanly.
     */
    for (unsigned int i = 0u; i < V3D_NOOP_CL_SIZE; i++){
        g_v3d_noop_cl[i] = 0u;
    }
    clean_data_cache_range((unsigned long)g_v3d_noop_cl, V3D_NOOP_CL_SIZE);
    start_bus = v3d_bus_address(g_v3d_noop_cl);
    end_bus = start_bus + 1u;

    v3d_write(cs_reg, V3D_CTRSTA);
    v3d_barrier();
    (void)v3d_wait_thread_stopped(cs_reg);

    v3d_write(ca_reg, start_bus);
    v3d_barrier();
    v3d_write(ea_reg, end_bus);
    v3d_barrier();

    rc = v3d_wait_thread_stopped(cs_reg);

    (void)v3d_get_status(&st);
    st.last_job_thread = thread;
    st.last_job_start_bus = start_bus;
    st.last_job_end_bus = end_bus;
    if (rc == 0){
        st.noop_count = ++g_v3d_noop_count;
    } else{
        v3d_write(cs_reg, V3D_CTRSTA);
        v3d_barrier();
        v3d_read_register_snapshot(&st);
        st.noop_count = g_v3d_noop_count;
        st.last_error = rc;
        st.fail_count = ++g_v3d_fail_count;
    }
    v3d_store_status(&st);

    spin_unlock(&g_v3d_lock);

    if (out){
        *out = st;
    }
    return rc;
}

int v3d_clear_visible(unsigned int rgba, qos_v3d_status_t* out){
    qos_v3d_status_t st;
    unsigned int page = fb_get_display_page();
    unsigned int width = fb_get_width();
    unsigned int height = fb_get_height();
    unsigned int pitch = fb_get_pitch();
    unsigned long fb_base = fb_get_page_base(page);
    unsigned long fb_bus = fb_get_page_bus_base(page);
    unsigned int tiles_x;
    unsigned int tiles_y;
    unsigned char* p = g_v3d_clear_cl;
    unsigned char* end = g_v3d_clear_cl + V3D_CLEAR_CL_SIZE;
    unsigned int start_bus;
    unsigned int end_bus;
    int rc = 0;

    if (width == 0u || height == 0u || pitch == 0u || fb_base == 0u || fb_bus == 0u){
        (void)v3d_get_status(&st);
        st.last_error = QOS_V3D_ERR_CONTROL;
        if (out){ *out = st; }
        return QOS_V3D_ERR_CONTROL;
    }

    if ((g_v3d_status.flags & QOS_V3D_FLAG_SCRATCH_OK) == 0u){
        rc = v3d_probe(&st);
        if (rc != 0){
            if (out){ *out = st; }
            return rc;
        }
    }

    tiles_x = (width + V3D_TILE_SIZE - 1u) / V3D_TILE_SIZE;
    tiles_y = (height + V3D_TILE_SIZE - 1u) / V3D_TILE_SIZE;
    if (tiles_x == 0u || tiles_y == 0u || tiles_x > 255u || tiles_y > 255u){
        (void)v3d_get_status(&st);
        st.last_error = QOS_V3D_ERR_CONTROL;
        if (out){ *out = st; }
        return QOS_V3D_ERR_CONTROL;
    }

    v3d_lock_init_once();
    spin_lock(&g_v3d_lock);

    for (unsigned int i = 0u; i < V3D_CLEAR_CL_SIZE; i++){
        g_v3d_clear_cl[i] = 0u;
    }

    v3d_emit_u8(&p, end, V3D_CL_RENDER_CONFIG);
    v3d_emit_u32(&p, end, (unsigned int)fb_bus);
    v3d_emit_u16(&p, end, width);
    v3d_emit_u16(&p, end, height);
    v3d_emit_u16(&p, end, V3D_RENDER_RGBA8888_LINEAR);

    v3d_emit_u8(&p, end, V3D_CL_CLEAR_COLORS);
    v3d_emit_u32(&p, end, rgba);
    v3d_emit_u32(&p, end, rgba);
    v3d_emit_u24(&p, end, 0u);
    v3d_emit_u8(&p, end, 0u);
    v3d_emit_u8(&p, end, 0u);

    /*
     * The tile buffer is cleared as part of the store/dump pipeline, so the
     * very first visible store after mode setup can contain stale tile-buffer
     * data. Prime it with one throwaway store to tile 0,0, then immediately
     * write tile 0,0 again during the real loop below.
     */
    v3d_emit_u8(&p, end, V3D_CL_TILE_COORDS);
    v3d_emit_u8(&p, end, 0u);
    v3d_emit_u8(&p, end, 0u);
    v3d_emit_u8(&p, end, V3D_CL_STORE_RESOLVED);

    for (unsigned int y = 0u; y < tiles_y; y++){
        for (unsigned int x = 0u; x < tiles_x; x++){
            int last = (x + 1u == tiles_x && y + 1u == tiles_y);
            v3d_emit_u8(&p, end, V3D_CL_TILE_COORDS);
            v3d_emit_u8(&p, end, x);
            v3d_emit_u8(&p, end, y);
            v3d_emit_u8(&p, end, last ? V3D_CL_STORE_EOF : V3D_CL_STORE_RESOLVED);
        }
    }
    v3d_emit_u8(&p, end, V3D_CL_HALT);

    if (p >= end){
        spin_unlock(&g_v3d_lock);
        (void)v3d_get_status(&st);
        st.last_error = QOS_V3D_ERR_CONTROL;
        if (out){ *out = st; }
        return QOS_V3D_ERR_CONTROL;
    }

    clean_data_cache_range((unsigned long)g_v3d_clear_cl,
                           (unsigned long)(p - g_v3d_clear_cl));
    clean_invalidate_data_cache_range(fb_base, (unsigned long)pitch * (unsigned long)height);
    start_bus = v3d_bus_address(g_v3d_clear_cl);
    end_bus = start_bus + (unsigned int)(p - g_v3d_clear_cl);

    v3d_write(V3D_CT1CS, V3D_CTRSTA);
    v3d_barrier();
    (void)v3d_wait_thread_stopped(V3D_CT1CS);
    v3d_write(V3D_CT1CA, start_bus);
    v3d_barrier();
    v3d_write(V3D_CT1EA, end_bus);
    v3d_barrier();

    rc = v3d_wait_thread_stopped(V3D_CT1CS);
    clean_invalidate_data_cache_range(fb_base, (unsigned long)pitch * (unsigned long)height);

    (void)v3d_get_status(&st);
    st.last_job_thread = 1u;
    st.last_job_start_bus = start_bus;
    st.last_job_end_bus = end_bus;
    st.last_clear_color = rgba;
    st.last_clear_page = page;
    st.last_clear_tiles = tiles_x * tiles_y;
    if (rc == 0){
        st.clear_count = ++g_v3d_clear_count;
    } else{
        v3d_write(V3D_CT1CS, V3D_CTRSTA);
        v3d_barrier();
        v3d_read_register_snapshot(&st);
        st.clear_count = g_v3d_clear_count;
        st.last_error = rc;
        st.fail_count = ++g_v3d_fail_count;
    }
    v3d_store_status(&st);
    spin_unlock(&g_v3d_lock);

    if (out){
        *out = st;
    }
    return rc;
}
