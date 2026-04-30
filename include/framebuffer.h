#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

void fb_init();
void fb_clear(unsigned int color);

void fb_init_buffers(void);
void fb_edit_buffer_pixel(unsigned int x, unsigned int y, unsigned int colour);
void fb_edit_buffer_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int colour);
void fb_update_buffer_pixels(void);

void fb_edit_buffer_pixel_fast(unsigned int x, unsigned int y, unsigned int colour);
void fb_update_buffer_pixels_fast(void);
void fb_edit_buffer_rect_fast(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int colour);


void fb_draw_pixel(unsigned int x, unsigned int y, unsigned int color);
void fb_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int color);

unsigned int fb_get_width();
unsigned int fb_get_height();

#endif
