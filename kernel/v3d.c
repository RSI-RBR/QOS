#include "v3d.h"
#include "mailbox.h"

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

#define V3D_EXPECTED_IDENT0  ((2u << 24) | ('D' << 16) | ('3' << 8) | 'V')
#define V3D_SCRATCH_TEST     0x51563344u

static qos_v3d_status_t g_v3d_status;
static unsigned int g_v3d_probe_count = 0u;
static unsigned int g_v3d_fail_count = 0u;

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

static void v3d_read_register_snapshot(qos_v3d_status_t* st){
    st->ident0 = v3d_read(V3D_IDENT0);
    st->ident1 = v3d_read(V3D_IDENT1);
    st->ident2 = v3d_read(V3D_IDENT2);
    st->ct0cs = v3d_read(V3D_CT0CS);
    st->ct1cs = v3d_read(V3D_CT1CS);
    st->intctl = v3d_read(V3D_INTCTL);
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
    st.intctl = 0u;
    st.clock_hz = 0u;
    st.probe_count = ++g_v3d_probe_count;
    st.fail_count = g_v3d_fail_count;
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
    v3d_store_status(&st);
    if (out){
        *out = st;
    }
    return st.last_error;
}
