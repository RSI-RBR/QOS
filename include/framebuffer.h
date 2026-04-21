#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

void fb_init();
void fb_clear(unsigned int color);

void fb_draw_pixel(int x, int y, unsigned int color);
void fb_draw_rect(int x, int y, int w, int h, unsigned int color);

unsigned int fb_get_width();
unsigned int fb_get_height();

#endif
