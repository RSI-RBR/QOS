#ifndef FB_CONSOLE_H
#define FB_CONSOLE_H

void fb_console_init(void);
void fb_console_clear(void);
void fb_console_putc(char c);
void fb_console_write(const char* s, unsigned long len);
void fb_console_load_screen(const unsigned char* cells,
                            unsigned int src_cols,
                            unsigned int src_rows,
                            unsigned int cursor_col,
                            unsigned int cursor_row);
void fb_console_render_rows(const unsigned char* cells,
                            unsigned int src_cols,
                            unsigned int src_rows,
                            unsigned int start_row,
                            unsigned int row_count,
                            unsigned int cursor_col,
                            unsigned int cursor_row);
void fb_console_set_colors(unsigned int fg, unsigned int bg, unsigned int cursor);
unsigned int fb_console_cols(void);
unsigned int fb_console_rows(void);

#endif
