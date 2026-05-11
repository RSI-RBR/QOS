#ifndef GPU2D_H
#define GPU2D_H

#define QOS_GPU2D_CAP_SOFTWARE_FALLBACK 0x00000001u
#define QOS_GPU2D_CAP_ACCEL_BLIT        0x00000002u
#define QOS_GPU2D_CAP_ACCEL_SCALE       0x00000004u
#define QOS_GPU2D_CAP_ACCEL_ALPHA       0x00000008u
#define QOS_GPU2D_CAP_ACCEL_ROTATE      0x00000010u
#define QOS_GPU2D_CAP_ACCEL_CLEAR       0x00000020u
#define QOS_GPU2D_CAP_ACCEL_FILL_TILE   0x00000040u
#define QOS_GPU2D_CAP_TEXTURE_OBJECTS   0x00000080u
#define QOS_GPU2D_CAP_QUAD_BATCH        0x00000100u
#define QOS_GPU2D_CAP_QPU_QUAD          0x00000200u
#define QOS_GPU2D_CAP_V3D_FILL_BATCH    0x00000400u

#define QOS_GPU2D_STATUS_READY          0x00010000u
#define QOS_GPU2D_STATUS_BACKEND_SOFT   0x00020000u
#define QOS_GPU2D_STATUS_BACKEND_HW     0x00040000u

#define QOS_GPU2D_QUAD_BATCH_MAX        256u
#define QOS_GPU2D_QUAD_FILL32           1u
#define QOS_GPU2D_QUAD_BLIT32           2u

#define QOS_GPU2D_BLIT_ALLOW_SOFTWARE   0x00000001u
#define QOS_GPU2D_BLIT_OPAQUE           0x00000002u
#define QOS_GPU2D_BLIT_BLEND            0x00000004u
#define QOS_GPU2D_BLIT_FLIP_X           0x00000008u
#define QOS_GPU2D_BLIT_FLIP_Y           0x00000010u

#define QOS_GPU2D_TEXTURE_OPAQUE        0x00000001u

typedef struct {
    const unsigned char* pixels;
    unsigned int texture_w;
    unsigned int texture_h;
    unsigned int pitch;
    unsigned int src_x;
    unsigned int src_y;
    unsigned int src_w;
    unsigned int src_h;
    int dst_x;
    int dst_y;
    unsigned int dst_w;
    unsigned int dst_h;
    unsigned int color_rgba;
    unsigned int flags;
} qos_gpu2d_blit_t;

typedef struct {
    const unsigned int* pixels;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int flags;
    unsigned int texture_id;
} qos_gpu2d_texture_upload_t;

typedef struct {
    unsigned int op;
    int x;
    int y;
    unsigned int w;
    unsigned int h;
    unsigned int color;
    const unsigned int* src;
    unsigned int src_pitch;
} qos_gpu2d_quad_t;

unsigned int gpu2d_status(void);
unsigned int gpu2d_blit_count(void);
unsigned int gpu2d_fallback_count(void);
unsigned int gpu2d_unsupported_count(void);
unsigned int gpu2d_clear_count(void);
unsigned int gpu2d_fill_count(void);
unsigned int gpu2d_quad_count(void);
unsigned int gpu2d_quad_batch_count(void);
unsigned int gpu2d_qpu_quad_count(void);
unsigned int gpu2d_qpu_fail_count(void);
unsigned int gpu2d_qpu_is_enabled(void);
int gpu2d_qpu_set_enabled(int enabled);
unsigned int gpu2d_v3d_quad_count(void);
unsigned int gpu2d_v3d_batch_count(void);
unsigned int gpu2d_v3d_fail_count(void);
unsigned int gpu2d_v3d_is_enabled(void);
int gpu2d_v3d_set_enabled(int enabled);
unsigned int gpu2d_texture_count(void);
unsigned int gpu2d_texture_upload_count(void);
unsigned int gpu2d_texture_free_count(void);
unsigned int gpu2d_texture_bytes(void);
int gpu2d_blit_rgba_for_pid(int pid, const qos_gpu2d_blit_t* blit);
int gpu2d_clear_for_pid(int pid, unsigned int color);
int gpu2d_fill_rect_for_pid(int pid,
                            unsigned int x,
                            unsigned int y,
                            unsigned int w,
                            unsigned int h,
                            unsigned int color);
int gpu2d_submit_quads_for_pid(int pid,
                               const qos_gpu2d_quad_t* quads,
                               unsigned int count);
int gpu2d_texture_upload_for_pid(int pid, qos_gpu2d_texture_upload_t* req);
int gpu2d_texture_free_for_pid(int pid, unsigned int texture_id);
void gpu2d_release_for_pid(int pid);

#endif
