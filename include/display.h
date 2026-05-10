#ifndef DISPLAY_H
#define DISPLAY_H

#define DISPLAY_MAX_SESSIONS 4
#define DISPLAY_TEXT_SESSION_ID 0

typedef enum {
    DISPLAY_NONE = 0,
    DISPLAY_TEXT = 1,
    DISPLAY_GRAPHICS = 2
} display_type_t;

typedef struct {
    int session_id;
    int owner_pid;
    display_type_t type;
    int active;
    void* framebuffer;
    unsigned long framebuffer_size;
    void* backing_framebuffer;
    unsigned long backing_framebuffer_size;
    void* allocation;
    unsigned long allocation_size;
    unsigned int dirty;
    unsigned int dirty_x0;
    unsigned int dirty_y0;
    unsigned int dirty_x1;
    unsigned int dirty_y1;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    int scanout_attached;
    unsigned int scanout_page;
} display_session_t;

void display_init(void);
int display_set_text_owner(int owner_pid);
int display_create_graphics_session(int owner_pid);
int display_destroy_session(int session_id);
void display_destroy_for_pid(int owner_pid);
int display_set_active(int session_id);
int display_set_active_for_pid(int owner_pid);
int display_get_active(void);
int display_get_for_pid(int owner_pid);
int display_get_info(int session_id, display_session_t* out);
void* display_get_framebuffer_for_pid(int owner_pid);
unsigned long display_get_framebuffer_size_for_pid(int owner_pid);
int display_mark_dirty_for_pid(int owner_pid);
int display_is_active_graphics_pid(int owner_pid);
int display_clear_for_pid(int owner_pid, unsigned int color);
int display_rect_for_pid(int owner_pid,
                         unsigned int x,
                         unsigned int y,
                         unsigned int w,
                         unsigned int h,
                         unsigned int color);
int display_blit_rgba32_for_pid(int owner_pid,
                                unsigned int x,
                                unsigned int y,
                                unsigned int w,
                                unsigned int h,
                                const unsigned char* rgba,
                                unsigned int rgba_pitch);
int display_blit_native32_for_pid(int owner_pid,
                                  unsigned int x,
                                  unsigned int y,
                                  unsigned int w,
                                  unsigned int h,
                                  const unsigned int* pixels,
                                  unsigned int pixels_pitch);
int display_present_for_pid(int owner_pid);
int display_present_active_graphics(void);
int display_present_active(void);
int display_gpu_set_enabled(int enabled);
unsigned int display_gpu_status(void);
unsigned int display_gpu_flip_count(void);
unsigned int display_gpu_failure_count(void);
void display_profile_reset(void);
void display_profile_dump(void);

#endif
