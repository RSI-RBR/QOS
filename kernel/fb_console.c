#include "fb_console.h"
#include "framebuffer.h"
#include "spinlock.h"

#define FB_CONSOLE_CELL_W 8u
#define FB_CONSOLE_CELL_H 16u
#define FB_CONSOLE_MAX_COLS (1920u / FB_CONSOLE_CELL_W)
#define FB_CONSOLE_MAX_ROWS (1080u / FB_CONSOLE_CELL_H)
#define FB_CONSOLE_MAX_PIXEL_W (FB_CONSOLE_MAX_COLS * FB_CONSOLE_CELL_W)
#define FB_CONSOLE_MAX_PIXEL_H (FB_CONSOLE_MAX_ROWS * FB_CONSOLE_CELL_H)

static unsigned char g_cells[FB_CONSOLE_MAX_ROWS][FB_CONSOLE_MAX_COLS];
static unsigned int g_pixels[FB_CONSOLE_MAX_PIXEL_H][FB_CONSOLE_MAX_PIXEL_W] __attribute__((aligned(64)));
static unsigned int g_cols = 0;
static unsigned int g_rows = 0;
static unsigned int g_cursor_col = 0;
static unsigned int g_cursor_row = 0;
static unsigned int g_fg = 0x00E8E2D0;
static unsigned int g_bg = 0x00000000;
static unsigned int g_cursor = 0x0040DDB8;
static int g_ready = 0;
static spinlock_t g_fb_console_lock;

static unsigned int console_pixel_width_locked(void){
    unsigned int w = g_cols * FB_CONSOLE_CELL_W;
    if (w > FB_CONSOLE_MAX_PIXEL_W){
        w = FB_CONSOLE_MAX_PIXEL_W;
    }
    return w;
}

static unsigned int console_pixel_height_locked(void){
    unsigned int h = g_rows * FB_CONSOLE_CELL_H;
    if (h > FB_CONSOLE_MAX_PIXEL_H){
        h = FB_CONSOLE_MAX_PIXEL_H;
    }
    return h;
}

static void back_draw_pixel_locked(unsigned int x, unsigned int y, unsigned int color){
    if (x >= console_pixel_width_locked() || y >= console_pixel_height_locked()){
        return;
    }
    g_pixels[y][x] = color;
}

static void back_draw_rect_locked(unsigned int x,
                                  unsigned int y,
                                  unsigned int w,
                                  unsigned int h,
                                  unsigned int color){
    unsigned int max_w = console_pixel_width_locked();
    unsigned int max_h = console_pixel_height_locked();

    if (x >= max_w || y >= max_h || w == 0u || h == 0u){
        return;
    }
    if (x + w < x || x + w > max_w){
        w = max_w - x;
    }
    if (y + h < y || y + h > max_h){
        h = max_h - y;
    }

    for (unsigned int py = y; py < y + h; py++){
        for (unsigned int px = x; px < x + w; px++){
            g_pixels[py][px] = color;
        }
    }
}

static void present_pixel_rows_locked(unsigned int start_y, unsigned int height){
    unsigned long base = fb_get_base();
    unsigned int pitch = fb_get_pitch();
    unsigned int fb_w = fb_get_width();
    unsigned int fb_h = fb_get_height();
    unsigned int width = console_pixel_width_locked();
    unsigned int max_h = console_pixel_height_locked();

    if (!base || pitch == 0u || width == 0u || height == 0u || start_y >= max_h){
        return;
    }
    if (width > fb_w){
        width = fb_w;
    }
    if (max_h > fb_h){
        max_h = fb_h;
    }
    if (start_y >= max_h){
        return;
    }
    if (start_y + height < start_y || start_y + height > max_h){
        height = max_h - start_y;
    }

    for (unsigned int y = 0; y < height; y++){
        unsigned int dst_y = start_y + y;
        unsigned int* dst = (unsigned int*)((unsigned char*)base + ((unsigned long)dst_y * pitch));
        unsigned int* src = &g_pixels[dst_y][0];
        for (unsigned int x = 0; x < width; x++){
            dst[x] = src[x];
        }
    }
}

static void present_console_rows_locked(unsigned int start_row, unsigned int row_count){
    if (row_count == 0u || start_row >= g_rows){
        return;
    }
    if (start_row + row_count < start_row || start_row + row_count > g_rows){
        row_count = g_rows - start_row;
    }
    present_pixel_rows_locked(start_row * FB_CONSOLE_CELL_H, row_count * FB_CONSOLE_CELL_H);
}

static void dirty_note_row_locked(unsigned int* start_row, unsigned int* end_row, unsigned int row){
    if (!start_row || !end_row || row >= g_rows){
        return;
    }
    if (row < *start_row){
        *start_row = row;
    }
    if (row > *end_row){
        *end_row = row;
    }
}

static void present_dirty_rows_locked(unsigned int start_row, unsigned int end_row){
    if (start_row >= g_rows || end_row >= g_rows || end_row < start_row){
        return;
    }
    present_console_rows_locked(start_row, (end_row - start_row) + 1u);
}

static void clear_visible_margins_locked(void){
    unsigned int w = console_pixel_width_locked();
    unsigned int h = console_pixel_height_locked();
    unsigned int fb_w = fb_get_width();
    unsigned int fb_h = fb_get_height();

    if (w < fb_w){
        fb_draw_rect(w, 0, fb_w - w, fb_h, g_bg);
    }
    if (h < fb_h){
        fb_draw_rect(0, h, fb_w, fb_h - h, g_bg);
    }
}

static void glyph_rows(unsigned char ch, unsigned char rows[7]){
    for (unsigned int i = 0; i < 7u; i++){
        rows[i] = 0u;
    }

#define GLYPH(a,b,c,d,e,f,g) do { \
    rows[0] = (a); rows[1] = (b); rows[2] = (c); rows[3] = (d); \
    rows[4] = (e); rows[5] = (f); rows[6] = (g); \
} while (0)

    switch (ch){
        case ' ': GLYPH(0x00,0x00,0x00,0x00,0x00,0x00,0x00); break;
        case '!': GLYPH(0x04,0x04,0x04,0x04,0x04,0x00,0x04); break;
        case '"': GLYPH(0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00); break;
        case '#': GLYPH(0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A); break;
        case '$': GLYPH(0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04); break;
        case '%': GLYPH(0x18,0x19,0x02,0x04,0x08,0x13,0x03); break;
        case '&': GLYPH(0x0C,0x12,0x14,0x08,0x15,0x12,0x0D); break;
        case '\'': GLYPH(0x04,0x04,0x08,0x00,0x00,0x00,0x00); break;
        case '(': GLYPH(0x02,0x04,0x08,0x08,0x08,0x04,0x02); break;
        case ')': GLYPH(0x08,0x04,0x02,0x02,0x02,0x04,0x08); break;
        case '*': GLYPH(0x00,0x04,0x15,0x0E,0x15,0x04,0x00); break;
        case '+': GLYPH(0x00,0x04,0x04,0x1F,0x04,0x04,0x00); break;
        case ',': GLYPH(0x00,0x00,0x00,0x00,0x04,0x04,0x08); break;
        case '-': GLYPH(0x00,0x00,0x00,0x1F,0x00,0x00,0x00); break;
        case '.': GLYPH(0x00,0x00,0x00,0x00,0x00,0x0C,0x0C); break;
        case '/': GLYPH(0x01,0x02,0x02,0x04,0x08,0x08,0x10); break;
        case '0': GLYPH(0x0E,0x11,0x13,0x15,0x19,0x11,0x0E); break;
        case '1': GLYPH(0x04,0x0C,0x04,0x04,0x04,0x04,0x0E); break;
        case '2': GLYPH(0x0E,0x11,0x01,0x02,0x04,0x08,0x1F); break;
        case '3': GLYPH(0x1F,0x02,0x04,0x02,0x01,0x11,0x0E); break;
        case '4': GLYPH(0x02,0x06,0x0A,0x12,0x1F,0x02,0x02); break;
        case '5': GLYPH(0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E); break;
        case '6': GLYPH(0x06,0x08,0x10,0x1E,0x11,0x11,0x0E); break;
        case '7': GLYPH(0x1F,0x01,0x02,0x04,0x08,0x08,0x08); break;
        case '8': GLYPH(0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E); break;
        case '9': GLYPH(0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C); break;
        case ':': GLYPH(0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00); break;
        case ';': GLYPH(0x00,0x0C,0x0C,0x00,0x04,0x04,0x08); break;
        case '<': GLYPH(0x02,0x04,0x08,0x10,0x08,0x04,0x02); break;
        case '=': GLYPH(0x00,0x00,0x1F,0x00,0x1F,0x00,0x00); break;
        case '>': GLYPH(0x08,0x04,0x02,0x01,0x02,0x04,0x08); break;
        case '?': GLYPH(0x0E,0x11,0x01,0x02,0x04,0x00,0x04); break;
        case '@': GLYPH(0x0E,0x11,0x17,0x15,0x17,0x10,0x0F); break;
        case 'A': case 'a': GLYPH(0x0E,0x11,0x11,0x1F,0x11,0x11,0x11); break;
        case 'B': case 'b': GLYPH(0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E); break;
        case 'C': case 'c': GLYPH(0x0E,0x11,0x10,0x10,0x10,0x11,0x0E); break;
        case 'D': case 'd': GLYPH(0x1E,0x11,0x11,0x11,0x11,0x11,0x1E); break;
        case 'E': case 'e': GLYPH(0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F); break;
        case 'F': case 'f': GLYPH(0x1F,0x10,0x10,0x1E,0x10,0x10,0x10); break;
        case 'G': case 'g': GLYPH(0x0E,0x11,0x10,0x17,0x11,0x11,0x0F); break;
        case 'H': case 'h': GLYPH(0x11,0x11,0x11,0x1F,0x11,0x11,0x11); break;
        case 'I': case 'i': GLYPH(0x0E,0x04,0x04,0x04,0x04,0x04,0x0E); break;
        case 'J': case 'j': GLYPH(0x01,0x01,0x01,0x01,0x11,0x11,0x0E); break;
        case 'K': case 'k': GLYPH(0x11,0x12,0x14,0x18,0x14,0x12,0x11); break;
        case 'L': case 'l': GLYPH(0x10,0x10,0x10,0x10,0x10,0x10,0x1F); break;
        case 'M': case 'm': GLYPH(0x11,0x1B,0x15,0x15,0x11,0x11,0x11); break;
        case 'N': case 'n': GLYPH(0x11,0x19,0x15,0x13,0x11,0x11,0x11); break;
        case 'O': case 'o': GLYPH(0x0E,0x11,0x11,0x11,0x11,0x11,0x0E); break;
        case 'P': case 'p': GLYPH(0x1E,0x11,0x11,0x1E,0x10,0x10,0x10); break;
        case 'Q': case 'q': GLYPH(0x0E,0x11,0x11,0x11,0x15,0x12,0x0D); break;
        case 'R': case 'r': GLYPH(0x1E,0x11,0x11,0x1E,0x14,0x12,0x11); break;
        case 'S': case 's': GLYPH(0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E); break;
        case 'T': case 't': GLYPH(0x1F,0x04,0x04,0x04,0x04,0x04,0x04); break;
        case 'U': case 'u': GLYPH(0x11,0x11,0x11,0x11,0x11,0x11,0x0E); break;
        case 'V': case 'v': GLYPH(0x11,0x11,0x11,0x11,0x0A,0x0A,0x04); break;
        case 'W': case 'w': GLYPH(0x11,0x11,0x11,0x15,0x15,0x15,0x0A); break;
        case 'X': case 'x': GLYPH(0x11,0x11,0x0A,0x04,0x0A,0x11,0x11); break;
        case 'Y': case 'y': GLYPH(0x11,0x11,0x0A,0x04,0x04,0x04,0x04); break;
        case 'Z': case 'z': GLYPH(0x1F,0x01,0x02,0x04,0x08,0x10,0x1F); break;
        case '[': GLYPH(0x0E,0x08,0x08,0x08,0x08,0x08,0x0E); break;
        case '\\': GLYPH(0x10,0x08,0x08,0x04,0x02,0x02,0x01); break;
        case ']': GLYPH(0x0E,0x02,0x02,0x02,0x02,0x02,0x0E); break;
        case '^': GLYPH(0x04,0x0A,0x11,0x00,0x00,0x00,0x00); break;
        case '_': GLYPH(0x00,0x00,0x00,0x00,0x00,0x00,0x1F); break;
        case '`': GLYPH(0x08,0x04,0x02,0x00,0x00,0x00,0x00); break;
        case '{': GLYPH(0x02,0x04,0x04,0x08,0x04,0x04,0x02); break;
        case '|': GLYPH(0x04,0x04,0x04,0x04,0x04,0x04,0x04); break;
        case '}': GLYPH(0x08,0x04,0x04,0x02,0x04,0x04,0x08); break;
        case '~': GLYPH(0x00,0x08,0x15,0x02,0x00,0x00,0x00); break;
        default: GLYPH(0x1F,0x11,0x02,0x04,0x04,0x00,0x04); break;
    }

#undef GLYPH
}

static void draw_cell_locked(unsigned int row, unsigned int col, int cursor_on){
    if (!g_ready || row >= g_rows || col >= g_cols){
        return;
    }

    unsigned int x0 = col * FB_CONSOLE_CELL_W;
    unsigned int y0 = row * FB_CONSOLE_CELL_H;
    unsigned char rows[7];
    unsigned char ch = g_cells[row][col];

    glyph_rows(ch, rows);
    back_draw_rect_locked(x0, y0, FB_CONSOLE_CELL_W, FB_CONSOLE_CELL_H, g_bg);

    for (unsigned int gy = 0; gy < 7u; gy++){
        for (unsigned int gx = 0; gx < 5u; gx++){
            if ((rows[gy] & (1u << (4u - gx))) == 0u){
                continue;
            }
            unsigned int px = x0 + 1u + gx;
            unsigned int py = y0 + 1u + gy * 2u;
            back_draw_pixel_locked(px, py, g_fg);
            back_draw_pixel_locked(px, py + 1u, g_fg);
        }
    }

    if (cursor_on){
        back_draw_rect_locked(x0, y0 + FB_CONSOLE_CELL_H - 2u, FB_CONSOLE_CELL_W, 2u, g_cursor);
    }
}

static void redraw_all_locked(void){
    if (!g_ready){
        return;
    }
    back_draw_rect_locked(0, 0, console_pixel_width_locked(), console_pixel_height_locked(), g_bg);
    for (unsigned int row = 0; row < g_rows; row++){
        for (unsigned int col = 0; col < g_cols; col++){
            draw_cell_locked(row, col, row == g_cursor_row && col == g_cursor_col);
        }
    }
    present_console_rows_locked(0, g_rows);
    clear_visible_margins_locked();
}

static void scroll_locked(void){
    if (g_rows == 0u || g_cols == 0u){
        return;
    }

    for (unsigned int row = 1u; row < g_rows; row++){
        for (unsigned int col = 0; col < g_cols; col++){
            g_cells[row - 1u][col] = g_cells[row][col];
        }
    }
    for (unsigned int col = 0; col < g_cols; col++){
        g_cells[g_rows - 1u][col] = ' ';
    }
    g_cursor_row = g_rows - 1u;
    redraw_all_locked();
}

static void newline_locked(void){
    g_cursor_col = 0;
    if (g_cursor_row + 1u >= g_rows){
        scroll_locked();
    } else{
        g_cursor_row++;
    }
}

static void putc_locked(char c){
    if (!g_ready){
        return;
    }

    unsigned int dirty_start = g_cursor_row;
    unsigned int dirty_end = g_cursor_row;
    draw_cell_locked(g_cursor_row, g_cursor_col, 0);

    if (c == '\n'){
        newline_locked();
        dirty_note_row_locked(&dirty_start, &dirty_end, g_cursor_row);
        draw_cell_locked(g_cursor_row, g_cursor_col, 1);
        present_dirty_rows_locked(dirty_start, dirty_end);
        return;
    }
    if (c == '\r'){
        g_cursor_col = 0;
        draw_cell_locked(g_cursor_row, g_cursor_col, 1);
        present_dirty_rows_locked(dirty_start, dirty_end);
        return;
    }
    if (c == '\b' || c == 0x7F){
        if (g_cursor_col > 0u){
            g_cursor_col--;
        } else if (g_cursor_row > 0u){
            g_cursor_row--;
            g_cursor_col = g_cols - 1u;
        }
        dirty_note_row_locked(&dirty_start, &dirty_end, g_cursor_row);
        g_cells[g_cursor_row][g_cursor_col] = ' ';
        draw_cell_locked(g_cursor_row, g_cursor_col, 1);
        present_dirty_rows_locked(dirty_start, dirty_end);
        return;
    }
    if (c == '\t'){
        do {
            putc_locked(' ');
        } while ((g_cursor_col & 3u) != 0u);
        return;
    }

    unsigned char ch = (unsigned char)c;
    if (ch < 0x20u || ch > 0x7Eu){
        ch = '?';
    }

    g_cells[g_cursor_row][g_cursor_col] = ch;
    draw_cell_locked(g_cursor_row, g_cursor_col, 0);
    g_cursor_col++;
    if (g_cursor_col >= g_cols){
        newline_locked();
    }
    dirty_note_row_locked(&dirty_start, &dirty_end, g_cursor_row);
    draw_cell_locked(g_cursor_row, g_cursor_col, 1);
    present_dirty_rows_locked(dirty_start, dirty_end);
}

void fb_console_init(void){
    spinlock_init(&g_fb_console_lock);
    g_cols = fb_get_width() / FB_CONSOLE_CELL_W;
    g_rows = fb_get_height() / FB_CONSOLE_CELL_H;
    if (g_cols > FB_CONSOLE_MAX_COLS){
        g_cols = FB_CONSOLE_MAX_COLS;
    }
    if (g_rows > FB_CONSOLE_MAX_ROWS){
        g_rows = FB_CONSOLE_MAX_ROWS;
    }
    if (g_cols == 0u){
        g_cols = 1u;
    }
    if (g_rows == 0u){
        g_rows = 1u;
    }
    g_ready = 1;
    fb_console_clear();
}

void fb_console_clear(void){
    unsigned long irq = spin_lock_irqsave(&g_fb_console_lock);
    for (unsigned int row = 0; row < FB_CONSOLE_MAX_ROWS; row++){
        for (unsigned int col = 0; col < FB_CONSOLE_MAX_COLS; col++){
            g_cells[row][col] = ' ';
        }
    }
    g_cursor_col = 0;
    g_cursor_row = 0;
    redraw_all_locked();
    spin_unlock_irqrestore(&g_fb_console_lock, irq);
}

void fb_console_putc(char c){
    unsigned long irq = spin_lock_irqsave(&g_fb_console_lock);
    putc_locked(c);
    spin_unlock_irqrestore(&g_fb_console_lock, irq);
}

void fb_console_write(const char* s, unsigned long len){
    if (!s){
        return;
    }
    unsigned long irq = spin_lock_irqsave(&g_fb_console_lock);
    for (unsigned long i = 0; i < len; i++){
        putc_locked(s[i]);
    }
    spin_unlock_irqrestore(&g_fb_console_lock, irq);
}

void fb_console_load_screen(const unsigned char* cells,
                            unsigned int src_cols,
                            unsigned int src_rows,
                            unsigned int cursor_col,
                            unsigned int cursor_row){
    if (!cells || src_cols == 0u || src_rows == 0u){
        return;
    }

    unsigned long irq = spin_lock_irqsave(&g_fb_console_lock);
    for (unsigned int row = 0; row < FB_CONSOLE_MAX_ROWS; row++){
        for (unsigned int col = 0; col < FB_CONSOLE_MAX_COLS; col++){
            unsigned char ch = ' ';
            if (row < g_rows && col < g_cols && row < src_rows && col < src_cols){
                ch = cells[(row * src_cols) + col];
                if (ch < 0x20u || ch > 0x7Eu){
                    ch = '?';
                }
            }
            g_cells[row][col] = ch;
        }
    }
    g_cursor_col = cursor_col;
    g_cursor_row = cursor_row;
    if (g_cursor_col >= g_cols){
        g_cursor_col = (g_cols > 0u) ? (g_cols - 1u) : 0u;
    }
    if (g_cursor_row >= g_rows){
        g_cursor_row = (g_rows > 0u) ? (g_rows - 1u) : 0u;
    }
    redraw_all_locked();
    spin_unlock_irqrestore(&g_fb_console_lock, irq);
}

void fb_console_render_rows(const unsigned char* cells,
                            unsigned int src_cols,
                            unsigned int src_rows,
                            unsigned int start_row,
                            unsigned int row_count,
                            unsigned int cursor_col,
                            unsigned int cursor_row){
    if (!cells || src_cols == 0u || src_rows == 0u || row_count == 0u){
        return;
    }

    unsigned long irq = spin_lock_irqsave(&g_fb_console_lock);
    if (!g_ready || start_row >= g_rows){
        spin_unlock_irqrestore(&g_fb_console_lock, irq);
        return;
    }

    unsigned int end_row = start_row + row_count;
    if (end_row < start_row || end_row > g_rows){
        end_row = g_rows;
    }

    g_cursor_col = cursor_col;
    g_cursor_row = cursor_row;
    if (g_cursor_col >= g_cols){
        g_cursor_col = (g_cols > 0u) ? (g_cols - 1u) : 0u;
    }
    if (g_cursor_row >= g_rows){
        g_cursor_row = (g_rows > 0u) ? (g_rows - 1u) : 0u;
    }

    for (unsigned int row = start_row; row < end_row; row++){
        for (unsigned int col = 0; col < g_cols; col++){
            unsigned char ch = ' ';
            if (row < src_rows && col < src_cols){
                ch = cells[(row * src_cols) + col];
                if (ch < 0x20u || ch > 0x7Eu){
                    ch = '?';
                }
            }
            g_cells[row][col] = ch;
        }
    }

    for (unsigned int row = start_row; row < end_row; row++){
        for (unsigned int col = 0; col < g_cols; col++){
            draw_cell_locked(row, col, row == g_cursor_row && col == g_cursor_col);
        }
    }
    present_console_rows_locked(start_row, end_row - start_row);
    spin_unlock_irqrestore(&g_fb_console_lock, irq);
}

void fb_console_set_colors(unsigned int fg, unsigned int bg, unsigned int cursor){
    unsigned long irq = spin_lock_irqsave(&g_fb_console_lock);
    g_fg = fg;
    g_bg = bg;
    g_cursor = cursor;
    redraw_all_locked();
    spin_unlock_irqrestore(&g_fb_console_lock, irq);
}

unsigned int fb_console_cols(void){
    return g_cols;
}

unsigned int fb_console_rows(void){
    return g_rows;
}
