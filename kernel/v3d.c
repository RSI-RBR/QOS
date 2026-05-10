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
#define V3D_L2CACTL          0x00020UL
#define V3D_SLCACTL          0x00024UL
#define V3D_INTCTL           0x00030UL
#define V3D_INTDIS           0x00038UL
#define V3D_SRQPC            0x00430UL
#define V3D_SRQUA            0x00434UL
#define V3D_SRQUL            0x00438UL
#define V3D_SRQCS            0x0043CUL
#define V3D_VPMBASE          0x00504UL
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
#define V3D_QPU_PROBE_MEM_SIZE 4096u
#define V3D_QPU_PROBE_MEM_ALIGN 4096u
#define V3D_QPU_WRITE_MAGIC0 0x51505531u
#define V3D_QPU_WRITE_MAGIC1 0x56433344u
#define V3D_QPU_EXEC_MEM_SIZE 4096u
#define V3D_QPU_EXEC_CODE_OFFSET 0x000u
#define V3D_QPU_EXEC_UNIFORM_OFFSET 0x400u
#define V3D_QPU_EXEC_INPUT_OFFSET 0x600u
#define V3D_QPU_EXEC_OUTPUT_OFFSET 0x800u
#define V3D_QPU_EXEC_WORDS 64u
#define V3D_QPU_EXEC_TIMEOUT 5000000u
#define V3D_QPU_EXEC_VPM_4KB 16u
#define V3D_QPU_COPY_MEM_SIZE 4096u
#define V3D_QPU_COPY_CODE_OFFSET 0x000u
#define V3D_QPU_COPY_UNIFORM_OFFSET 0x400u
#define V3D_QPU_COPY_FILL_OFFSET 0xC00u
#define V3D_QPU_COPY_ROW_WORDS 64u
#define V3D_QPU_COPY_ROW_BYTES (V3D_QPU_COPY_ROW_WORDS * sizeof(unsigned int))
#define V3D_QPU_COPY_BATCH_ROWS 8u
#define V3D_QPU_COPY_TIMEOUT 5000000u
#define V3D_MEM_FLAG_DIRECT   (1u << 2)
#define V3D_MEM_FLAG_ZERO     (1u << 4)
#define V3D_MEM_FLAG_HINT_PERMALOCK (1u << 6)
#define V3D_QPU_MEM_FLAGS     (V3D_MEM_FLAG_DIRECT | V3D_MEM_FLAG_ZERO | V3D_MEM_FLAG_HINT_PERMALOCK)
#define V3D_CL_HALT           0u
#define V3D_CL_STORE_RESOLVED 24u
#define V3D_CL_STORE_EOF      25u
#define V3D_CL_RENDER_CONFIG  113u
#define V3D_CL_CLEAR_COLORS   114u
#define V3D_CL_TILE_COORDS    115u
#define V3D_RENDER_RGBA8888_LINEAR (1u << 2)
#define V3D_SRQCS_RESET       ((1u << 7) | (1u << 8) | (1u << 16))
#define V3D_INTCTL_QPU_DONE   (1u << 2)

static const unsigned int g_v3d_qpu_dma_copy_code[] = {
    0x8304080f, 0xe0020c67,
    0x15800dc0, 0xd0020ca7,
    0x15ca7c00, 0x100209e7,
    0x80900078, 0xe0020827,
    0x15800dc0, 0xd0020867,
    0x00000000, 0xe00208a7,
    0x0d9c45c0, 0xd00228e7,
    0x00000048, 0xf02809e7,
    0x009e7000, 0x100009e7,
    0x009e7000, 0x100009e7,
    0x009e7000, 0x100009e7,
    0x159e7000, 0x10021c67,
    0x159e7240, 0x10021ca7,
    0x159f2e00, 0x100209e7,
    0x00000800, 0xe00208e7,
    0x0c9e70c0, 0x10020827,
    0xffffff90, 0xf0f809e7,
    0x00000040, 0xe00208e7,
    0x0c9e72c0, 0x10020867,
    0x0c9c15c0, 0xd00208a7,
    0x159c1fc0, 0xd00209a7,
    0x009e7000, 0x300009e7,
    0x009e7000, 0x100009e7,
    0x009e7000, 0x100009e7
};

static qos_v3d_status_t g_v3d_status;
static unsigned int g_v3d_probe_count = 0u;
static unsigned int g_v3d_fail_count = 0u;
static unsigned int g_v3d_noop_count = 0u;
static unsigned int g_v3d_clear_count = 0u;
static unsigned int g_v3d_qpu_probe_count = 0u;
static unsigned int g_v3d_qpu_write_count = 0u;
static unsigned int g_v3d_qpu_exec_count = 0u;
static unsigned char g_v3d_noop_cl[V3D_NOOP_CL_SIZE] __attribute__((aligned(64)));
static unsigned char g_v3d_clear_cl[V3D_CLEAR_CL_SIZE] __attribute__((aligned(64)));
static spinlock_t g_v3d_lock;
static int g_v3d_lock_ready = 0;
static unsigned int g_v3d_qpu_copy_handle = 0u;
static unsigned int g_v3d_qpu_copy_bus = 0u;
static unsigned long g_v3d_qpu_copy_arm = 0UL;

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

static unsigned long v3d_arm_address_from_vc_bus(unsigned int bus){
    return (unsigned long)(bus & 0x3FFFFFFFu);
}

static int v3d_vc_memory_arm_accessible(unsigned long arm, unsigned int size){
    if (arm == 0UL || size == 0u){
        return 0;
    }
    if ((arm & 3UL) != 0UL){
        return 0;
    }
    if (arm + (unsigned long)size < arm){
        return 0;
    }
    /*
     * Pi 3 GPU memory lives in the same low SDRAM window when locked through
     * the firmware. Keep this conservative so we never probe MMIO by mistake.
     */
    if (arm + (unsigned long)size > 0x3F000000UL){
        return 0;
    }
    return 1;
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

static void v3d_clean_invalidate_frame_tiles(unsigned long fb_base,
                                             unsigned int pitch,
                                             unsigned int tile_x,
                                             unsigned int tile_y,
                                             unsigned int tile_w,
                                             unsigned int tile_h,
                                             unsigned int fb_width,
                                             unsigned int fb_height){
    unsigned int x0 = tile_x * V3D_TILE_SIZE;
    unsigned int y0 = tile_y * V3D_TILE_SIZE;
    unsigned int x1 = (tile_x + tile_w) * V3D_TILE_SIZE;
    unsigned int y1 = (tile_y + tile_h) * V3D_TILE_SIZE;
    if (x1 > fb_width){
        x1 = fb_width;
    }
    if (y1 > fb_height){
        y1 = fb_height;
    }
    if (x1 <= x0 || y1 <= y0){
        return;
    }
    if (x0 == 0u && y0 == 0u && x1 == fb_width && y1 == fb_height){
        clean_invalidate_data_cache_range(fb_base,
                                          (unsigned long)pitch * (unsigned long)fb_height);
        return;
    }
    unsigned int row_bytes = (x1 - x0) * sizeof(unsigned int);
    for (unsigned int y = y0; y < y1; y++){
        clean_invalidate_data_cache_range(fb_base +
                                          ((unsigned long)y * pitch) +
                                          ((unsigned long)x0 * sizeof(unsigned int)),
                                          row_bytes);
    }
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
    st.qpu_probe_count = g_v3d_qpu_probe_count;
    st.qpu_write_count = g_v3d_qpu_write_count;
    st.qpu_exec_count = g_v3d_qpu_exec_count;
    st.last_job_thread = g_v3d_status.last_job_thread;
    st.last_job_start_bus = g_v3d_status.last_job_start_bus;
    st.last_job_end_bus = g_v3d_status.last_job_end_bus;
    st.last_clear_color = g_v3d_status.last_clear_color;
    st.last_clear_page = g_v3d_status.last_clear_page;
    st.last_clear_tiles = g_v3d_status.last_clear_tiles;
    st.last_qpu_handle = g_v3d_status.last_qpu_handle;
    st.last_qpu_bus = g_v3d_status.last_qpu_bus;
    st.last_qpu_size = g_v3d_status.last_qpu_size;
    st.last_qpu_rc = g_v3d_status.last_qpu_rc;
    st.last_qpu_arm = g_v3d_status.last_qpu_arm;
    st.last_qpu_write0 = g_v3d_status.last_qpu_write0;
    st.last_qpu_read0 = g_v3d_status.last_qpu_read0;
    st.last_qpu_write1 = g_v3d_status.last_qpu_write1;
    st.last_qpu_read1 = g_v3d_status.last_qpu_read1;
    st.last_qpu_write_rc = g_v3d_status.last_qpu_write_rc;
    st.last_qpu_exec_status = g_v3d_status.last_qpu_exec_status;
    st.last_qpu_exec_mismatch = g_v3d_status.last_qpu_exec_mismatch;
    st.last_qpu_exec_expected0 = g_v3d_status.last_qpu_exec_expected0;
    st.last_qpu_exec_result0 = g_v3d_status.last_qpu_exec_result0;
    st.last_qpu_exec_expected63 = g_v3d_status.last_qpu_exec_expected63;
    st.last_qpu_exec_result63 = g_v3d_status.last_qpu_exec_result63;
    st.last_qpu_exec_rc = g_v3d_status.last_qpu_exec_rc;
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
        st.qpu_probe_count = g_v3d_qpu_probe_count;
        st.qpu_write_count = g_v3d_qpu_write_count;
        st.qpu_exec_count = g_v3d_qpu_exec_count;
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
    st.qpu_probe_count = g_v3d_qpu_probe_count;
    st.qpu_write_count = g_v3d_qpu_write_count;
    st.qpu_exec_count = g_v3d_qpu_exec_count;
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

int v3d_qpu_memory_probe(qos_v3d_status_t* out){
    qos_v3d_status_t st;
    unsigned int handle = 0u;
    unsigned int bus = 0u;
    int rc = 0;

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

    (void)v3d_get_status(&st);
    st.qpu_probe_count = ++g_v3d_qpu_probe_count;
    st.last_qpu_handle = 0u;
    st.last_qpu_bus = 0u;
    st.last_qpu_size = V3D_QPU_PROBE_MEM_SIZE;
    st.last_qpu_rc = 0;

    if (mailbox_set_qpu_enabled(1u) != 0){
        rc = QOS_V3D_ERR_QPU_MEMORY;
        st.last_qpu_rc = rc;
    } else{
        st.flags |= QOS_V3D_FLAG_QPU_OK;
    }

    if (rc == 0 &&
        mailbox_alloc_vc_memory(V3D_QPU_PROBE_MEM_SIZE,
                                V3D_QPU_PROBE_MEM_ALIGN,
                                V3D_QPU_MEM_FLAGS,
                                &handle) != 0){
        rc = QOS_V3D_ERR_QPU_MEMORY;
        st.last_qpu_rc = rc;
    }

    if (rc == 0 &&
        mailbox_lock_vc_memory(handle, &bus) != 0){
        rc = QOS_V3D_ERR_QPU_MEMORY;
        st.last_qpu_rc = rc;
    }

    if (rc == 0){
        st.flags |= QOS_V3D_FLAG_QPU_MEM_OK;
        st.last_qpu_handle = handle;
        st.last_qpu_bus = bus;
        st.last_qpu_rc = 0;
    }

    if (bus != 0u){
        if (mailbox_unlock_vc_memory(handle) != 0 && rc == 0){
            rc = QOS_V3D_ERR_QPU_MEMORY;
            st.last_qpu_rc = rc;
        }
    }
    if (handle != 0u){
        if (mailbox_release_vc_memory(handle) != 0 && rc == 0){
            rc = QOS_V3D_ERR_QPU_MEMORY;
            st.last_qpu_rc = rc;
        }
    }

    if (rc != 0){
        st.last_error = rc;
        st.fail_count = ++g_v3d_fail_count;
    } else{
        st.last_error = 0;
        st.fail_count = g_v3d_fail_count;
    }

    v3d_read_register_snapshot(&st);
    v3d_store_status(&st);
    spin_unlock(&g_v3d_lock);

    if (out){
        *out = st;
    }
    return rc;
}

int v3d_qpu_memory_write_probe(qos_v3d_status_t* out){
    qos_v3d_status_t st;
    unsigned int handle = 0u;
    unsigned int bus = 0u;
    unsigned long arm = 0UL;
    unsigned int wrote0 = V3D_QPU_WRITE_MAGIC0;
    unsigned int wrote1 = V3D_QPU_WRITE_MAGIC1;
    unsigned int read0 = 0u;
    unsigned int read1 = 0u;
    int rc = 0;

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

    (void)v3d_get_status(&st);
    st.qpu_write_count = ++g_v3d_qpu_write_count;
    st.last_qpu_handle = 0u;
    st.last_qpu_bus = 0u;
    st.last_qpu_arm = 0u;
    st.last_qpu_size = V3D_QPU_PROBE_MEM_SIZE;
    st.last_qpu_rc = 0;
    st.last_qpu_write0 = wrote0;
    st.last_qpu_write1 = wrote1;
    st.last_qpu_read0 = 0u;
    st.last_qpu_read1 = 0u;
    st.last_qpu_write_rc = 0;

    if (mailbox_set_qpu_enabled(1u) != 0){
        rc = QOS_V3D_ERR_QPU_MEMORY;
        st.last_qpu_rc = rc;
        st.last_qpu_write_rc = rc;
    } else{
        st.flags |= QOS_V3D_FLAG_QPU_OK;
    }

    if (rc == 0 &&
        mailbox_alloc_vc_memory(V3D_QPU_PROBE_MEM_SIZE,
                                V3D_QPU_PROBE_MEM_ALIGN,
                                V3D_QPU_MEM_FLAGS,
                                &handle) != 0){
        rc = QOS_V3D_ERR_QPU_MEMORY;
        st.last_qpu_rc = rc;
        st.last_qpu_write_rc = rc;
    }

    if (rc == 0 &&
        mailbox_lock_vc_memory(handle, &bus) != 0){
        rc = QOS_V3D_ERR_QPU_MEMORY;
        st.last_qpu_rc = rc;
        st.last_qpu_write_rc = rc;
    }

    if (rc == 0){
        arm = v3d_arm_address_from_vc_bus(bus);
        st.last_qpu_handle = handle;
        st.last_qpu_bus = bus;
        st.last_qpu_arm = (unsigned int)arm;
        if (!v3d_vc_memory_arm_accessible(arm, V3D_QPU_PROBE_MEM_SIZE)){
            rc = QOS_V3D_ERR_QPU_WRITE;
            st.last_qpu_rc = rc;
            st.last_qpu_write_rc = rc;
        }
    }

    if (rc == 0){
        volatile unsigned int* words = (volatile unsigned int*)arm;
        words[0] = wrote0;
        words[1] = wrote1;
        words[2] = bus;
        words[3] = V3D_QPU_PROBE_MEM_SIZE;
        asm volatile("dsb sy" ::: "memory");
        clean_invalidate_data_cache_range(arm, 64UL);
        read0 = words[0];
        read1 = words[1];
        st.last_qpu_read0 = read0;
        st.last_qpu_read1 = read1;

        if (read0 == wrote0 && read1 == wrote1){
            st.flags |= QOS_V3D_FLAG_QPU_MEM_OK | QOS_V3D_FLAG_QPU_WRITE_OK;
            st.last_qpu_write_rc = 0;
        } else{
            rc = QOS_V3D_ERR_QPU_WRITE;
            st.last_qpu_rc = rc;
            st.last_qpu_write_rc = rc;
        }

        for (unsigned int i = 0u; i < 16u; i++){
            words[i] = 0u;
        }
        asm volatile("dsb sy" ::: "memory");
        clean_data_cache_range(arm, 64UL);
    }

    if (bus != 0u){
        if (mailbox_unlock_vc_memory(handle) != 0 && rc == 0){
            rc = QOS_V3D_ERR_QPU_MEMORY;
            st.last_qpu_rc = rc;
            st.last_qpu_write_rc = rc;
        }
    }
    if (handle != 0u){
        if (mailbox_release_vc_memory(handle) != 0 && rc == 0){
            rc = QOS_V3D_ERR_QPU_MEMORY;
            st.last_qpu_rc = rc;
            st.last_qpu_write_rc = rc;
        }
    }

    if (rc != 0){
        st.last_error = rc;
        st.fail_count = ++g_v3d_fail_count;
    } else{
        st.last_error = 0;
        st.last_qpu_rc = 0;
        st.fail_count = g_v3d_fail_count;
    }

    v3d_read_register_snapshot(&st);
    v3d_store_status(&st);
    spin_unlock(&g_v3d_lock);

    if (out){
        *out = st;
    }
    return rc;
}

int v3d_qpu_execute_probe(qos_v3d_status_t* out){
    qos_v3d_status_t st;
    unsigned int handle = 0u;
    unsigned int bus = 0u;
    unsigned long arm = 0UL;
    unsigned int old_vpmbase = 0u;
    unsigned int srqcs = 0u;
    unsigned int mismatch = 0u;
    int launched = 0;
    int completed = 0;
    int keep_allocation = 0;
    int rc = 0;

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

    (void)v3d_get_status(&st);
    st.qpu_exec_count = ++g_v3d_qpu_exec_count;
    st.last_qpu_handle = 0u;
    st.last_qpu_bus = 0u;
    st.last_qpu_arm = 0u;
    st.last_qpu_size = V3D_QPU_EXEC_MEM_SIZE;
    st.last_qpu_exec_status = 0u;
    st.last_qpu_exec_mismatch = 0u;
    st.last_qpu_exec_expected0 = 0u;
    st.last_qpu_exec_result0 = 0u;
    st.last_qpu_exec_expected63 = 0u;
    st.last_qpu_exec_result63 = 0u;
    st.last_qpu_exec_rc = 0;

    if (mailbox_set_qpu_enabled(1u) != 0){
        rc = QOS_V3D_ERR_QPU_MEMORY;
        st.last_qpu_rc = rc;
        st.last_qpu_exec_rc = rc;
    } else{
        st.flags |= QOS_V3D_FLAG_QPU_OK;
    }

    if (rc == 0 &&
        mailbox_alloc_vc_memory(V3D_QPU_EXEC_MEM_SIZE,
                                V3D_QPU_PROBE_MEM_ALIGN,
                                V3D_QPU_MEM_FLAGS,
                                &handle) != 0){
        rc = QOS_V3D_ERR_QPU_MEMORY;
        st.last_qpu_rc = rc;
        st.last_qpu_exec_rc = rc;
    }

    if (rc == 0 &&
        mailbox_lock_vc_memory(handle, &bus) != 0){
        rc = QOS_V3D_ERR_QPU_MEMORY;
        st.last_qpu_rc = rc;
        st.last_qpu_exec_rc = rc;
    }

    if (rc == 0){
        arm = v3d_arm_address_from_vc_bus(bus);
        st.last_qpu_handle = handle;
        st.last_qpu_bus = bus;
        st.last_qpu_arm = (unsigned int)arm;
        if (!v3d_vc_memory_arm_accessible(arm, V3D_QPU_EXEC_MEM_SIZE)){
            rc = QOS_V3D_ERR_QPU_WRITE;
            st.last_qpu_rc = rc;
            st.last_qpu_exec_rc = rc;
        }
    }

    if (rc == 0){
        volatile unsigned int* words = (volatile unsigned int*)arm;
        unsigned int code_words =
            (unsigned int)(sizeof(g_v3d_qpu_dma_copy_code) /
                           sizeof(g_v3d_qpu_dma_copy_code[0]));
        unsigned int code_base = V3D_QPU_EXEC_CODE_OFFSET / sizeof(unsigned int);
        unsigned int uniform_base = V3D_QPU_EXEC_UNIFORM_OFFSET / sizeof(unsigned int);
        unsigned int input_base = V3D_QPU_EXEC_INPUT_OFFSET / sizeof(unsigned int);
        unsigned int output_base = V3D_QPU_EXEC_OUTPUT_OFFSET / sizeof(unsigned int);

        for (unsigned int i = 0u; i < V3D_QPU_EXEC_MEM_SIZE / sizeof(unsigned int); i++){
            words[i] = 0u;
        }

        for (unsigned int i = 0u; i < code_words; i++){
            words[code_base + i] = g_v3d_qpu_dma_copy_code[i];
        }

        words[uniform_base + 0u] = bus + V3D_QPU_EXEC_INPUT_OFFSET;
        words[uniform_base + 1u] = bus + V3D_QPU_EXEC_OUTPUT_OFFSET;

        for (unsigned int i = 0u; i < V3D_QPU_EXEC_WORDS; i++){
            unsigned int value = 0x51500000u + i;
            words[input_base + i] = value;
            words[output_base + i] = 0xDEAD0000u + i;
        }

        st.last_qpu_exec_expected0 = words[input_base];
        st.last_qpu_exec_expected63 = words[input_base + V3D_QPU_EXEC_WORDS - 1u];

        asm volatile("dsb sy" ::: "memory");
        clean_data_cache_range(arm, V3D_QPU_EXEC_MEM_SIZE);

        (void)v3d_wait_thread_stopped(V3D_CT0CS);
        (void)v3d_wait_thread_stopped(V3D_CT1CS);

        old_vpmbase = v3d_read(V3D_VPMBASE);
        v3d_write(V3D_VPMBASE, V3D_QPU_EXEC_VPM_4KB);
        v3d_write(V3D_L2CACTL, 4u);
        v3d_write(V3D_SLCACTL, 0xFFFFFFFFu);
        v3d_write(V3D_INTCTL, V3D_INTCTL_QPU_DONE);
        v3d_write(V3D_SRQCS, V3D_SRQCS_RESET);
        v3d_barrier();

        v3d_write(V3D_SRQUL, 2u);
        v3d_write(V3D_SRQUA, bus + V3D_QPU_EXEC_UNIFORM_OFFSET);
        v3d_barrier();
        v3d_write(V3D_SRQPC, bus + V3D_QPU_EXEC_CODE_OFFSET);
        launched = 1;
        v3d_barrier();

        rc = QOS_V3D_ERR_TIMEOUT;
        for (unsigned int i = 0u; i < V3D_QPU_EXEC_TIMEOUT; i++){
            srqcs = v3d_read(V3D_SRQCS);
            if (((srqcs >> 16) & 0xFFu) >= 1u){
                rc = 0;
                completed = 1;
                break;
            }
            if ((i & 0x3FFu) == 0u){
                asm volatile("yield" ::: "memory");
            }
        }

        st.last_qpu_exec_status = srqcs;
        if (completed){
            v3d_write(V3D_INTCTL, V3D_INTCTL_QPU_DONE);
            v3d_write(V3D_VPMBASE, old_vpmbase);
            v3d_barrier();

            clean_invalidate_data_cache_range(arm + V3D_QPU_EXEC_OUTPUT_OFFSET,
                                              V3D_QPU_EXEC_WORDS * sizeof(unsigned int));
            st.last_qpu_exec_result0 = words[output_base];
            st.last_qpu_exec_result63 = words[output_base + V3D_QPU_EXEC_WORDS - 1u];
            for (unsigned int i = 0u; i < V3D_QPU_EXEC_WORDS; i++){
                if (words[output_base + i] != words[input_base + i]){
                    mismatch++;
                }
            }
            st.last_qpu_exec_mismatch = mismatch;
            if (mismatch == 0u){
                st.flags |= QOS_V3D_FLAG_QPU_MEM_OK |
                            QOS_V3D_FLAG_QPU_WRITE_OK |
                            QOS_V3D_FLAG_QPU_EXEC_OK;
                st.last_qpu_exec_rc = 0;
                st.last_qpu_rc = 0;
            } else{
                rc = QOS_V3D_ERR_QPU_EXEC;
                st.last_qpu_exec_rc = rc;
                st.last_qpu_rc = rc;
            }
        } else{
            keep_allocation = launched ? 1 : 0;
            st.last_qpu_exec_rc = rc;
            st.last_qpu_rc = rc;
        }

        if (!keep_allocation){
            for (unsigned int i = 0u; i < V3D_QPU_EXEC_MEM_SIZE / sizeof(unsigned int); i++){
                words[i] = 0u;
            }
            asm volatile("dsb sy" ::: "memory");
            clean_data_cache_range(arm, V3D_QPU_EXEC_MEM_SIZE);
        }
    }

    if (bus != 0u && !keep_allocation){
        if (mailbox_unlock_vc_memory(handle) != 0 && rc == 0){
            rc = QOS_V3D_ERR_QPU_MEMORY;
            st.last_qpu_rc = rc;
            st.last_qpu_exec_rc = rc;
        }
    }
    if (handle != 0u && !keep_allocation){
        if (mailbox_release_vc_memory(handle) != 0 && rc == 0){
            rc = QOS_V3D_ERR_QPU_MEMORY;
            st.last_qpu_rc = rc;
            st.last_qpu_exec_rc = rc;
        }
    }

    if (rc != 0){
        st.last_error = rc;
        st.fail_count = ++g_v3d_fail_count;
    } else{
        st.last_error = 0;
        st.fail_count = g_v3d_fail_count;
    }

    v3d_read_register_snapshot(&st);
    v3d_store_status(&st);
    spin_unlock(&g_v3d_lock);

    if (out){
        *out = st;
    }
    return rc;
}

static int v3d_qpu_copy_engine_init_locked(void){
    qos_v3d_status_t st;
    unsigned int handle = 0u;
    unsigned int bus = 0u;
    unsigned long arm = 0UL;

    if (g_v3d_qpu_copy_handle != 0u &&
        g_v3d_qpu_copy_bus != 0u &&
        g_v3d_qpu_copy_arm != 0UL){
        return 0;
    }

    if ((g_v3d_status.flags & QOS_V3D_FLAG_SCRATCH_OK) == 0u){
        int rc = v3d_probe(&st);
        if (rc != 0){
            return rc;
        }
    }

    if (mailbox_set_qpu_enabled(1u) != 0){
        return QOS_V3D_ERR_QPU_MEMORY;
    }

    if (mailbox_alloc_vc_memory(V3D_QPU_COPY_MEM_SIZE,
                                V3D_QPU_PROBE_MEM_ALIGN,
                                V3D_QPU_MEM_FLAGS,
                                &handle) != 0){
        return QOS_V3D_ERR_QPU_MEMORY;
    }

    if (mailbox_lock_vc_memory(handle, &bus) != 0){
        (void)mailbox_release_vc_memory(handle);
        return QOS_V3D_ERR_QPU_MEMORY;
    }

    arm = v3d_arm_address_from_vc_bus(bus);
    if (!v3d_vc_memory_arm_accessible(arm, V3D_QPU_COPY_MEM_SIZE)){
        (void)mailbox_unlock_vc_memory(handle);
        (void)mailbox_release_vc_memory(handle);
        return QOS_V3D_ERR_QPU_WRITE;
    }

    volatile unsigned int* words = (volatile unsigned int*)arm;
    unsigned int code_words =
        (unsigned int)(sizeof(g_v3d_qpu_dma_copy_code) /
                       sizeof(g_v3d_qpu_dma_copy_code[0]));
    unsigned int code_base = V3D_QPU_COPY_CODE_OFFSET / sizeof(unsigned int);
    unsigned int fill_base = V3D_QPU_COPY_FILL_OFFSET / sizeof(unsigned int);

    for (unsigned int i = 0u; i < V3D_QPU_COPY_MEM_SIZE / sizeof(unsigned int); i++){
        words[i] = 0u;
    }
    for (unsigned int i = 0u; i < code_words; i++){
        words[code_base + i] = g_v3d_qpu_dma_copy_code[i];
    }
    for (unsigned int i = 0u; i < V3D_QPU_COPY_ROW_WORDS; i++){
        words[fill_base + i] = 0u;
    }
    asm volatile("dsb sy" ::: "memory");
    clean_data_cache_range(arm, V3D_QPU_COPY_MEM_SIZE);

    g_v3d_qpu_copy_handle = handle;
    g_v3d_qpu_copy_bus = bus;
    g_v3d_qpu_copy_arm = arm;

    (void)v3d_get_status(&st);
    st.flags |= QOS_V3D_FLAG_QPU_OK |
                QOS_V3D_FLAG_QPU_MEM_OK |
                QOS_V3D_FLAG_QPU_WRITE_OK |
                QOS_V3D_FLAG_QPU_EXEC_OK;
    st.last_qpu_handle = handle;
    st.last_qpu_bus = bus;
    st.last_qpu_arm = (unsigned int)arm;
    st.last_qpu_size = V3D_QPU_COPY_MEM_SIZE;
    st.last_qpu_rc = 0;
    st.last_qpu_exec_rc = 0;
    v3d_store_status(&st);
    return 0;
}

static int v3d_qpu_run_copy64_batch_locked(unsigned int src_bus,
                                           unsigned int src_pitch,
                                           unsigned int dst_bus,
                                           unsigned int dst_pitch,
                                           unsigned int rows){
    if (rows == 0u || rows > V3D_QPU_COPY_BATCH_ROWS){
        return QOS_V3D_ERR_CONTROL;
    }

    int rc = v3d_qpu_copy_engine_init_locked();
    if (rc != 0){
        return rc;
    }

    volatile unsigned int* words = (volatile unsigned int*)g_v3d_qpu_copy_arm;
    unsigned int uniform_base = V3D_QPU_COPY_UNIFORM_OFFSET / sizeof(unsigned int);
    unsigned int old_vpmbase = v3d_read(V3D_VPMBASE);
    unsigned int srqcs = 0u;

    for (unsigned int row = 0u; row < rows; row++){
        unsigned int row_src_bus = src_bus + (row * src_pitch);
        unsigned int row_dst_bus = dst_bus + (row * dst_pitch);
        words[uniform_base + (row * 2u) + 0u] = row_src_bus;
        words[uniform_base + (row * 2u) + 1u] = row_dst_bus;
        clean_data_cache_range((unsigned long)(row_src_bus & 0x3FFFFFFFu),
                               V3D_QPU_COPY_ROW_BYTES);
        clean_invalidate_data_cache_range((unsigned long)(row_dst_bus & 0x3FFFFFFFu),
                                          V3D_QPU_COPY_ROW_BYTES);
    }
    asm volatile("dsb sy" ::: "memory");
    clean_data_cache_range(g_v3d_qpu_copy_arm + V3D_QPU_COPY_UNIFORM_OFFSET,
                           (unsigned long)rows * 2UL * sizeof(unsigned int));

    (void)v3d_wait_thread_stopped(V3D_CT0CS);
    (void)v3d_wait_thread_stopped(V3D_CT1CS);

    v3d_write(V3D_VPMBASE, V3D_QPU_EXEC_VPM_4KB);
    v3d_write(V3D_L2CACTL, 4u);
    v3d_write(V3D_SLCACTL, 0xFFFFFFFFu);
    v3d_write(V3D_INTCTL, V3D_INTCTL_QPU_DONE);
    v3d_write(V3D_SRQCS, V3D_SRQCS_RESET);
    v3d_barrier();

    v3d_write(V3D_SRQUL, 2u);
    v3d_write(V3D_SRQUA, g_v3d_qpu_copy_bus + V3D_QPU_COPY_UNIFORM_OFFSET);
    v3d_barrier();
    for (unsigned int row = 0u; row < rows; row++){
        v3d_write(V3D_SRQPC, g_v3d_qpu_copy_bus + V3D_QPU_COPY_CODE_OFFSET);
        v3d_barrier();
    }

    rc = QOS_V3D_ERR_TIMEOUT;
    for (unsigned int i = 0u; i < V3D_QPU_COPY_TIMEOUT; i++){
        srqcs = v3d_read(V3D_SRQCS);
        if (((srqcs >> 16) & 0xFFu) >= rows){
            rc = 0;
            break;
        }
        if ((i & 0x3FFu) == 0u){
            asm volatile("yield" ::: "memory");
        }
    }

    v3d_write(V3D_INTCTL, V3D_INTCTL_QPU_DONE);
    v3d_write(V3D_VPMBASE, old_vpmbase);
    v3d_barrier();

    for (unsigned int row = 0u; row < rows; row++){
        unsigned int row_dst_bus = dst_bus + (row * dst_pitch);
        clean_invalidate_data_cache_range((unsigned long)(row_dst_bus & 0x3FFFFFFFu),
                                          V3D_QPU_COPY_ROW_BYTES);
    }

    if (rc != 0){
        qos_v3d_status_t st;
        (void)v3d_get_status(&st);
        st.last_qpu_exec_status = srqcs;
        st.last_qpu_exec_rc = rc;
        st.last_qpu_rc = rc;
        st.last_error = rc;
        st.fail_count = ++g_v3d_fail_count;
        v3d_store_status(&st);
    }
    return rc;
}

static int v3d_qpu_copy64_rows_locked(unsigned int src_bus,
                                      unsigned int src_pitch,
                                      unsigned int dst_bus,
                                      unsigned int dst_pitch,
                                      unsigned int rows){
    if (src_bus == 0u || dst_bus == 0u || rows == 0u){
        return QOS_V3D_ERR_CONTROL;
    }

    unsigned int done = 0u;
    while (done < rows){
        unsigned int chunk = rows - done;
        if (chunk > V3D_QPU_COPY_BATCH_ROWS){
            chunk = V3D_QPU_COPY_BATCH_ROWS;
        }
        int rc = v3d_qpu_run_copy64_batch_locked(src_bus + (done * src_pitch),
                                                 src_pitch,
                                                 dst_bus + (done * dst_pitch),
                                                 dst_pitch,
                                                 chunk);
        if (rc != 0){
            return rc;
        }
        done += chunk;
    }
    return 0;
}

int v3d_qpu_copy64_rows(unsigned int src_bus,
                        unsigned int src_pitch,
                        unsigned int dst_bus,
                        unsigned int dst_pitch,
                        unsigned int rows){
    if (rows == 0u || rows > 4096u){
        return QOS_V3D_ERR_CONTROL;
    }

    v3d_lock_init_once();
    spin_lock(&g_v3d_lock);
    int rc = v3d_qpu_copy64_rows_locked(src_bus,
                                        src_pitch,
                                        dst_bus,
                                        dst_pitch,
                                        rows);
    spin_unlock(&g_v3d_lock);
    return rc;
}

int v3d_qpu_fill64_rows(unsigned int dst_bus,
                        unsigned int dst_pitch,
                        unsigned int rows,
                        unsigned int color){
    if (dst_bus == 0u || rows == 0u || rows > 4096u){
        return QOS_V3D_ERR_CONTROL;
    }

    v3d_lock_init_once();
    spin_lock(&g_v3d_lock);
    int rc = v3d_qpu_copy_engine_init_locked();
    if (rc == 0){
        volatile unsigned int* words = (volatile unsigned int*)g_v3d_qpu_copy_arm;
        unsigned int fill_base = V3D_QPU_COPY_FILL_OFFSET / sizeof(unsigned int);
        for (unsigned int i = 0u; i < V3D_QPU_COPY_ROW_WORDS; i++){
            words[fill_base + i] = color;
        }
        asm volatile("dsb sy" ::: "memory");
        clean_data_cache_range(g_v3d_qpu_copy_arm + V3D_QPU_COPY_FILL_OFFSET,
                               V3D_QPU_COPY_ROW_BYTES);
        rc = v3d_qpu_copy64_rows_locked(g_v3d_qpu_copy_bus + V3D_QPU_COPY_FILL_OFFSET,
                                        0u,
                                        dst_bus,
                                        dst_pitch,
                                        rows);
    }
    spin_unlock(&g_v3d_lock);
    return rc;
}

int v3d_clear_page_tiles(unsigned int page,
                         unsigned int rgba,
                         unsigned int tile_x,
                         unsigned int tile_y,
                         unsigned int tile_w,
                         unsigned int tile_h,
                         qos_v3d_status_t* out){
    qos_v3d_status_t st;
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
    if (tiles_x == 0u || tiles_y == 0u || tiles_x > 255u || tiles_y > 255u ||
        tile_w == 0u || tile_h == 0u ||
        tile_x >= tiles_x || tile_y >= tiles_y ||
        tile_x + tile_w < tile_x || tile_y + tile_h < tile_y ||
        tile_x + tile_w > tiles_x || tile_y + tile_h > tiles_y){
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

    /*
     * The VC4 docs describe clear-colors as optionally preceding the tile
     * rendering configuration. Emitting it first ensures the tile buffer gets
     * initialized with our requested color before the first tile is stored.
     */
    v3d_emit_u8(&p, end, V3D_CL_CLEAR_COLORS);
    v3d_emit_u32(&p, end, rgba);
    v3d_emit_u32(&p, end, rgba);
    v3d_emit_u24(&p, end, 0u);
    v3d_emit_u8(&p, end, 0u);
    v3d_emit_u8(&p, end, 0u);

    v3d_emit_u8(&p, end, V3D_CL_RENDER_CONFIG);
    v3d_emit_u32(&p, end, (unsigned int)fb_bus);
    v3d_emit_u16(&p, end, width);
    v3d_emit_u16(&p, end, height);
    v3d_emit_u16(&p, end, V3D_RENDER_RGBA8888_LINEAR);

    /*
     * First pass primes the tile-buffer clear state. On Pi 3, the first tile
     * can otherwise be stale after coming from unrelated GPU work. The second
     * pass is the visible clear and signals end-of-frame on the final tile.
     */
    for (unsigned int pass = 0u; pass < 2u; pass++){
        for (unsigned int y = tile_y; y < tile_y + tile_h; y++){
            for (unsigned int x = tile_x; x < tile_x + tile_w; x++){
                int last = (pass == 1u &&
                            x + 1u == tile_x + tile_w &&
                            y + 1u == tile_y + tile_h);
                v3d_emit_u8(&p, end, V3D_CL_TILE_COORDS);
                v3d_emit_u8(&p, end, x);
                v3d_emit_u8(&p, end, y);
                v3d_emit_u8(&p, end, last ? V3D_CL_STORE_EOF : V3D_CL_STORE_RESOLVED);
            }
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
    v3d_clean_invalidate_frame_tiles(fb_base,
                                     pitch,
                                     tile_x,
                                     tile_y,
                                     tile_w,
                                     tile_h,
                                     width,
                                     height);
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
    v3d_clean_invalidate_frame_tiles(fb_base,
                                     pitch,
                                     tile_x,
                                     tile_y,
                                     tile_w,
                                     tile_h,
                                     width,
                                     height);

    (void)v3d_get_status(&st);
    st.last_job_thread = 1u;
    st.last_job_start_bus = start_bus;
    st.last_job_end_bus = end_bus;
    st.last_clear_color = rgba;
    st.last_clear_page = page;
    st.last_clear_tiles = tile_w * tile_h;
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

int v3d_clear_page(unsigned int page, unsigned int rgba, qos_v3d_status_t* out){
    unsigned int width = fb_get_width();
    unsigned int height = fb_get_height();
    unsigned int tiles_x = (width + V3D_TILE_SIZE - 1u) / V3D_TILE_SIZE;
    unsigned int tiles_y = (height + V3D_TILE_SIZE - 1u) / V3D_TILE_SIZE;
    return v3d_clear_page_tiles(page, rgba, 0u, 0u, tiles_x, tiles_y, out);
}

int v3d_clear_visible(unsigned int rgba, qos_v3d_status_t* out){
    return v3d_clear_page(fb_get_display_page(), rgba, out);
}
