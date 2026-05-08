#ifndef FB_CONSOLE_H
#define FB_CONSOLE_H

void fb_console_init(void);
void fb_console_clear(void);
void fb_console_putc(char c);
void fb_console_write(const char* s, unsigned long len);
void fb_console_set_colors(unsigned int fg, unsigned int bg, unsigned int cursor);
unsigned int fb_console_cols(void);
unsigned int fb_console_rows(void);

#endif
