#ifndef V3D_H
#define V3D_H

#define QOS_V3D_FLAG_PROBED      0x00000001u
#define QOS_V3D_FLAG_CLOCK_OK    0x00000002u
#define QOS_V3D_FLAG_PRESENT     0x00000004u
#define QOS_V3D_FLAG_IDENT_OK    0x00000008u
#define QOS_V3D_FLAG_SCRATCH_OK  0x00000010u
#define QOS_V3D_FLAG_QPU_OK      0x00000020u

#define QOS_V3D_ERR_NOT_PROBED   (-1)
#define QOS_V3D_ERR_CLOCK        (-2)
#define QOS_V3D_ERR_IDENT        (-3)
#define QOS_V3D_ERR_SCRATCH      (-4)
#define QOS_V3D_ERR_CONTROL      (-5)
#define QOS_V3D_ERR_TIMEOUT      (-6)

typedef struct {
    unsigned int flags;
    unsigned int ident0;
    unsigned int ident1;
    unsigned int ident2;
    unsigned int scratch_before;
    unsigned int scratch_after;
    unsigned int ct0cs;
    unsigned int ct1cs;
    unsigned int ct0ca;
    unsigned int ct1ca;
    unsigned int ct0ea;
    unsigned int ct1ea;
    unsigned int intctl;
    unsigned int errstat;
    unsigned int clock_hz;
    unsigned int probe_count;
    unsigned int fail_count;
    unsigned int noop_count;
    unsigned int clear_count;
    unsigned int last_job_thread;
    unsigned int last_job_start_bus;
    unsigned int last_job_end_bus;
    unsigned int last_clear_color;
    unsigned int last_clear_page;
    unsigned int last_clear_tiles;
    int last_error;
} qos_v3d_status_t;

int v3d_probe(qos_v3d_status_t* out);
int v3d_get_status(qos_v3d_status_t* out);
int v3d_submit_noop(unsigned int thread, qos_v3d_status_t* out);
int v3d_clear_page_tiles(unsigned int page,
                         unsigned int rgba,
                         unsigned int tile_x,
                         unsigned int tile_y,
                         unsigned int tile_w,
                         unsigned int tile_h,
                         qos_v3d_status_t* out);
int v3d_clear_page(unsigned int page, unsigned int rgba, qos_v3d_status_t* out);
int v3d_clear_visible(unsigned int rgba, qos_v3d_status_t* out);

#endif
