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

typedef struct {
    unsigned int flags;
    unsigned int ident0;
    unsigned int ident1;
    unsigned int ident2;
    unsigned int scratch_before;
    unsigned int scratch_after;
    unsigned int ct0cs;
    unsigned int ct1cs;
    unsigned int intctl;
    unsigned int clock_hz;
    unsigned int probe_count;
    unsigned int fail_count;
    int last_error;
} qos_v3d_status_t;

int v3d_probe(qos_v3d_status_t* out);
int v3d_get_status(qos_v3d_status_t* out);

#endif
