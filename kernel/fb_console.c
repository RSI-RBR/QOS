#include "fb_console.h"
#include "framebuffer.h"
#include "spinlock.h"

#define FB_CONSOLE_CELL_W 8u
#define FB_CONSOLE_CELL_H 16u
#define FB_CONSOLE_MAX_COLS (1920u / FB_CONSOLE_CELL_W)
#define FB_CONSOLE_MAX_ROWS (1080u / FB_CONSOLE_CELL_H)

static unsigned char g_cells[FB_CONSOLE_MAX_ROWS][FB_CONSOLE_MAX_COLS];
static unsigned int g_cols = 0;
static unsigned int g_rows = 0;
static unsigned int g_cursor_col = 0;
static unsigned int g_cursor_row = 0;
static unsigned int g_fg = 0x00E8E2D0;
static unsigned int g_bg = 0x00000000;
static unsigned int g_cursor = 0x0040DDB8;
static int g_ready = 0;
static spinlock_t g_fb_console_lock;

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
    fb_draw_rect(x0, y0, FB_CONSOLE_CELL_W, FB_CONSOLE_CELL_H, g_bg);

    for (unsigned int gy = 0; gy < 7u; gy++){
        for (unsigned int gx = 0; gx < 5u; gx++){
            if ((rows[gy] & (1u << (4u - gx))) == 0u){
                continue;
            }
            unsigned int px = x0 + 1u + gx;
            unsigned int py = y0 + 1u + gy * 2u;
            fb_draw_pixel(px, py, g_fg);
            fb_draw_pixel(px, py + 1u, g_fg);
        }
    }

    if (cursor_on){
        fb_draw_rect(x0, y0 + FB_CONSOLE_CELL_H - 2u, FB_CONSOLE_CELL_W, 2u, g_cursor);
    }
}

static void redraw_all_locked(void){
    if (!g_ready){
        return;
    }
    fb_draw_rect(0, 0, fb_get_width(), fb_get_height(), g_bg);
    for (unsigned int row = 0; row < g_rows; row++){
        for (unsigned int col = 0; col < g_cols; col++){
            draw_cell_locked(row, col, row == g_cursor_row && col == g_cursor_col);
        }
    }
}

static int scroll_pixels_locked(void){
    unsigned long base = fb_get_base();
    unsigned int pitch = fb_get_pitch();
    unsigned int width = fb_get_width();
    unsigned int height = fb_get_height();
    if (!base || pitch == 0u || width == 0u || height <= FB_CONSOLE_CELL_H){
        return -1;
    }

    for (unsigned int y = 0; y + FB_CONSOLE_CELL_H < height; y++){
        unsigned int* dst = (unsigned int*)((unsigned char*)base + ((unsigned long)y * pitch));
        unsigned int* src = (unsigned int*)((unsigned char*)base + ((unsigned long)(y + FB_CONSOLE_CELL_H) * pitch));
        for (unsigned int x = 0; x < width; x++){
            dst[x] = src[x];
        }
    }

    unsigned int clear_y = height - FB_CONSOLE_CELL_H;
    fb_draw_rect(0, clear_y, width, FB_CONSOLE_CELL_H, g_bg);
    return 0;
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
    if (scroll_pixels_locked() != 0){
        redraw_all_locked();
    }
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

    draw_cell_locked(g_cursor_row, g_cursor_col, 0);

    if (c == '\n'){
        newline_locked();
        draw_cell_locked(g_cursor_row, g_cursor_col, 1);
        return;
    }
    if (c == '\r'){
        g_cursor_col = 0;
        draw_cell_locked(g_cursor_row, g_cursor_col, 1);
        return;
    }
    if (c == '\b' || c == 0x7F){
        if (g_cursor_col > 0u){
            g_cursor_col--;
        } else if (g_cursor_row > 0u){
            g_cursor_row--;
            g_cursor_col = g_cols - 1u;
        }
        g_cells[g_cursor_row][g_cursor_col] = ' ';
        draw_cell_locked(g_cursor_row, g_cursor_col, 1);
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
    draw_cell_locked(g_cursor_row, g_cursor_col, 1);
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
