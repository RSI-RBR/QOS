#ifndef GPU2D_H
#define GPU2D_H

#define QOS_GPU2D_CAP_SOFTWARE_FALLBACK 0x00000001u
#define QOS_GPU2D_CAP_ACCEL_BLIT        0x00000002u
#define QOS_GPU2D_CAP_ACCEL_SCALE       0x00000004u
#define QOS_GPU2D_CAP_ACCEL_ALPHA       0x00000008u
#define QOS_GPU2D_CAP_ACCEL_ROTATE      0x00000010u
#define QOS_GPU2D_CAP_ACCEL_CLEAR       0x00000020u
#define QOS_GPU2D_CAP_ACCEL_FILL_TILE   0x00000040u

#define QOS_GPU2D_STATUS_READY          0x00010000u
#define QOS_GPU2D_STATUS_BACKEND_SOFT   0x00020000u
#define QOS_GPU2D_STATUS_BACKEND_HW     0x00040000u

#define QOS_GPU2D_BLIT_ALLOW_SOFTWARE   0x00000001u
#define QOS_GPU2D_BLIT_OPAQUE           0x00000002u
#define QOS_GPU2D_BLIT_BLEND            0x00000004u
#define QOS_GPU2D_BLIT_FLIP_X           0x00000008u
#define QOS_GPU2D_BLIT_FLIP_Y           0x00000010u

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

unsigned int gpu2d_status(void);
unsigned int gpu2d_blit_count(void);
unsigned int gpu2d_fallback_count(void);
unsigned int gpu2d_unsupported_count(void);
unsigned int gpu2d_clear_count(void);
unsigned int gpu2d_fill_count(void);
int gpu2d_blit_rgba_for_pid(int pid, const qos_gpu2d_blit_t* blit);
int gpu2d_clear_for_pid(int pid, unsigned int color);
int gpu2d_fill_rect_for_pid(int pid,
                            unsigned int x,
                            unsigned int y,
                            unsigned int w,
                            unsigned int h,
                            unsigned int color);

#endif
