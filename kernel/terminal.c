#include "terminal.h"
#include "fb_console.h"
#include "uart.h"
#include "console.h"
#include "remote_login.h"
#include "spinlock.h"
#include "display.h"

#define TERM_FLAG_UART QOS_TERM_OUTPUT_UART
#define TERM_FLAG_FB   QOS_TERM_OUTPUT_FB
#define TERM_PID_MAP_MAX 64
#define TERM_MAX_COLS 240u
#define TERM_MAX_ROWS 67u
#define TERM_INPUT_QUEUE_LEN 256u

typedef struct {
    int id;
    int foreground_pid;
    unsigned int flags;
    unsigned int cols;
    unsigned int rows;
    unsigned int cursor_col;
    unsigned int cursor_row;
    unsigned char cells[TERM_MAX_ROWS][TERM_MAX_COLS];
    unsigned char in_q[TERM_INPUT_QUEUE_LEN];
    unsigned char in_src_q[TERM_INPUT_QUEUE_LEN];
    unsigned int in_head;
    unsigned int in_tail;
    unsigned int in_count;
    int login_required;
    int login_authenticated;
} terminal_t;

static terminal_t g_terms[QOS_TERMINAL_MAX];
static signed char g_pid_term[TERM_PID_MAP_MAX];
static int g_active_term = 0;
static spinlock_t g_terminal_lock;
static spinlock_t g_terminal_render_lock;
static unsigned char g_render_cells[TERM_MAX_ROWS][TERM_MAX_COLS];

static int terminal_valid_id(int term_id){
    return term_id >= 0 && term_id < QOS_TERMINAL_MAX;
}

static terminal_t* terminal_get_locked(int term_id){
    if (!terminal_valid_id(term_id)){
        return 0;
    }
    return &g_terms[term_id];
}

static int terminal_get_for_pid_locked(int pid){
    if (pid >= 0 && pid < TERM_PID_MAP_MAX){
        int term_id = (int)g_pid_term[pid];
        if (terminal_valid_id(term_id)){
            return term_id;
        }
    }
    return g_active_term;
}

static void terminal_clear_buffer_locked(terminal_t* term){
    if (!term){
        return;
    }
    for (unsigned int row = 0; row < TERM_MAX_ROWS; row++){
        for (unsigned int col = 0; col < TERM_MAX_COLS; col++){
            term->cells[row][col] = ' ';
        }
    }
    term->cursor_col = 0;
    term->cursor_row = 0;
}

static void terminal_render_rows_unlocked(int term_id,
                                          unsigned int start_row,
                                          unsigned int end_row){
    if (!terminal_valid_id(term_id)){
        return;
    }
    if (display_get_active() != DISPLAY_TEXT_SESSION_ID){
        return;
    }

    if (!spin_trylock(&g_terminal_render_lock)){
        return;
    }

    unsigned int cursor_col = 0u;
    unsigned int cursor_row = 0u;
    unsigned int row_count = 0u;
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(term_id);
    if (!term || term->cols == 0u || term->rows == 0u || start_row >= term->rows){
        spin_unlock_irqrestore(&g_terminal_lock, irq);
        spin_unlock(&g_terminal_render_lock);
        return;
    }
    if (end_row >= term->rows){
        end_row = term->rows - 1u;
    }
    if (end_row < start_row){
        spin_unlock_irqrestore(&g_terminal_lock, irq);
        spin_unlock(&g_terminal_render_lock);
        return;
    }
    for (unsigned int row = start_row; row <= end_row; row++){
        for (unsigned int col = 0; col < term->cols; col++){
            g_render_cells[row][col] = term->cells[row][col];
        }
    }
    cursor_col = term->cursor_col;
    cursor_row = term->cursor_row;
    row_count = (end_row - start_row) + 1u;
    spin_unlock_irqrestore(&g_terminal_lock, irq);

    fb_console_render_rows(&g_render_cells[0][0],
                           TERM_MAX_COLS,
                           TERM_MAX_ROWS,
                           start_row,
                           row_count,
                           cursor_col,
                           cursor_row);
    spin_unlock(&g_terminal_render_lock);
}

static void terminal_render_all_unlocked(int term_id){
    terminal_render_rows_unlocked(term_id, 0u, TERM_MAX_ROWS - 1u);
}

static void terminal_dirty_note(unsigned int* start_row, unsigned int* end_row, unsigned int row){
    if (!start_row || !end_row){
        return;
    }
    if (row < *start_row){
        *start_row = row;
    }
    if (row > *end_row){
        *end_row = row;
    }
}

static int terminal_char_will_scroll(const terminal_t* term, char c){
    if (!term || term->rows == 0u || term->cols == 0u || term->cursor_row + 1u < term->rows){
        return 0;
    }
    if (c == '\n'){
        return 1;
    }
    if (c == '\t'){
        unsigned int spaces = 4u - (term->cursor_col & 3u);
        return (term->cursor_col + spaces >= term->cols) ? 1 : 0;
    }
    if ((unsigned char)c >= 0x20u && (unsigned char)c <= 0x7Eu && term->cursor_col + 1u >= term->cols){
        return 1;
    }
    return 0;
}

static void terminal_scroll_locked(terminal_t* term){
    if (!term || term->cols == 0u || term->rows == 0u){
        return;
    }

    for (unsigned int row = 1u; row < term->rows; row++){
        for (unsigned int col = 0; col < term->cols; col++){
            term->cells[row - 1u][col] = term->cells[row][col];
        }
    }
    for (unsigned int col = 0; col < term->cols; col++){
        term->cells[term->rows - 1u][col] = ' ';
    }
    term->cursor_row = term->rows - 1u;
}

static void terminal_newline_locked(terminal_t* term){
    if (!term){
        return;
    }
    term->cursor_col = 0;
    if (term->cursor_row + 1u >= term->rows){
        terminal_scroll_locked(term);
    } else{
        term->cursor_row++;
    }
}

static void terminal_buffer_putc_locked(terminal_t* term, char c){
    if (!term || term->cols == 0u || term->rows == 0u){
        return;
    }

    if (c == '\n'){
        terminal_newline_locked(term);
        return;
    }
    if (c == '\r'){
        term->cursor_col = 0;
        return;
    }
    if (c == '\b' || c == 0x7F){
        if (term->cursor_col > 0u){
            term->cursor_col--;
        } else if (term->cursor_row > 0u){
            term->cursor_row--;
            term->cursor_col = term->cols - 1u;
        }
        term->cells[term->cursor_row][term->cursor_col] = ' ';
        return;
    }
    if (c == '\t'){
        do {
            terminal_buffer_putc_locked(term, ' ');
        } while ((term->cursor_col & 3u) != 0u);
        return;
    }

    unsigned char ch = (unsigned char)c;
    if (ch < 0x20u || ch > 0x7Eu){
        ch = '?';
    }
    term->cells[term->cursor_row][term->cursor_col] = ch;
    term->cursor_col++;
    if (term->cursor_col >= term->cols){
        terminal_newline_locked(term);
    }
}

static int terminal_queue_push_locked(terminal_t* term, char c, unsigned int src){
    if (!term || term->in_count >= TERM_INPUT_QUEUE_LEN){
        return 0;
    }
    term->in_q[term->in_tail] = (unsigned char)c;
    term->in_src_q[term->in_tail] = (unsigned char)(src & 0xFFu);
    term->in_tail = (term->in_tail + 1u) % TERM_INPUT_QUEUE_LEN;
    term->in_count++;
    return 1;
}

static int terminal_queue_pop_locked(terminal_t* term, char* out, unsigned int* out_source){
    if (!term || !out || term->in_count == 0u){
        return 0;
    }
    *out = (char)term->in_q[term->in_head];
    if (out_source){
        *out_source = (unsigned int)term->in_src_q[term->in_head];
    }
    term->in_head = (term->in_head + 1u) % TERM_INPUT_QUEUE_LEN;
    term->in_count--;
    return 1;
}

static int terminal_pid_is_foreground_locked(int term_id, int pid){
    terminal_t* term = terminal_get_locked(term_id);
    if (!term || pid < 0){
        return 0;
    }
    return term->foreground_pid == pid;
}

void terminal_init(void){
    spinlock_init(&g_terminal_lock);
    spinlock_init(&g_terminal_render_lock);
    fb_console_init();
    unsigned int cols = fb_console_cols();
    unsigned int rows = fb_console_rows();
    if (cols == 0u || cols > TERM_MAX_COLS){
        cols = TERM_MAX_COLS;
    }
    if (rows == 0u || rows > TERM_MAX_ROWS){
        rows = TERM_MAX_ROWS;
    }

    for (int i = 0; i < QOS_TERMINAL_MAX; i++){
        g_terms[i].id = i;
        g_terms[i].foreground_pid = -1;
        g_terms[i].flags = TERM_FLAG_UART;
        g_terms[i].cols = cols;
        g_terms[i].rows = rows;
        g_terms[i].in_head = 0;
        g_terms[i].in_tail = 0;
        g_terms[i].in_count = 0;
        g_terms[i].login_required = 0;
        g_terms[i].login_authenticated = 0;
        terminal_clear_buffer_locked(&g_terms[i]);
    }
    for (int i = 0; i < TERM_PID_MAP_MAX; i++){
        g_pid_term[i] = -1;
    }
    g_active_term = 0;
    g_terms[0].flags = TERM_FLAG_UART | TERM_FLAG_FB;
    terminal_render_all_unlocked(0);
}

void terminal_clear_active(void){
    int term_id = -1;
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(g_active_term);
    if (term){
        terminal_clear_buffer_locked(term);
        if ((term->flags & TERM_FLAG_FB) &&
            display_get_active() == DISPLAY_TEXT_SESSION_ID){
            term_id = g_active_term;
        }
    }
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    if (term_id >= 0){
        terminal_render_all_unlocked(term_id);
    }
}

int terminal_attach_pid(int pid, int term_id){
    if (pid < 0 || pid >= TERM_PID_MAP_MAX || !terminal_valid_id(term_id)){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    int old_term = (int)g_pid_term[pid];
    if (terminal_valid_id(old_term) && g_terms[old_term].foreground_pid == pid){
        g_terms[old_term].foreground_pid = -1;
        for (int i = 0; i < TERM_PID_MAP_MAX; i++){
            if (i != pid && g_pid_term[i] == (signed char)old_term){
                g_terms[old_term].foreground_pid = i;
                break;
            }
        }
    }
    g_pid_term[pid] = (signed char)term_id;
    if (g_terms[term_id].foreground_pid < 0){
        g_terms[term_id].foreground_pid = pid;
    }
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return 0;
}

void terminal_detach_pid(int pid){
    if (pid < 0 || pid >= TERM_PID_MAP_MAX){
        return;
    }
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    int term_id = (int)g_pid_term[pid];
    if (terminal_valid_id(term_id) && g_terms[term_id].foreground_pid == pid){
        g_terms[term_id].foreground_pid = -1;
        for (int i = 0; i < TERM_PID_MAP_MAX; i++){
            if (g_pid_term[i] == (signed char)term_id){
                g_terms[term_id].foreground_pid = i;
                break;
            }
        }
    }
    g_pid_term[pid] = -1;
    spin_unlock_irqrestore(&g_terminal_lock, irq);
}

int terminal_set_foreground_pid(int term_id, int pid){
    if (!terminal_valid_id(term_id) || pid < 0 || pid >= TERM_PID_MAP_MAX){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    if (g_pid_term[pid] != (signed char)term_id){
        spin_unlock_irqrestore(&g_terminal_lock, irq);
        return -1;
    }
    g_terms[term_id].foreground_pid = pid;
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return 0;
}

int terminal_get_foreground_pid(int term_id){
    if (!terminal_valid_id(term_id)){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    int pid = g_terms[term_id].foreground_pid;
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return pid;
}

int terminal_get_for_pid(int pid){
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    int term_id = terminal_get_for_pid_locked(pid);
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return term_id;
}

void terminal_putc(int term_id, int pid, char c){
    int owner = (pid >= 0) ? console_get_owner() : -1;
    int do_render = 0;
    unsigned int dirty_start = 0u;
    unsigned int dirty_end = 0u;
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(term_id);
    int active = (term_id == g_active_term);
    int mirror_uart = !term || (term->flags & TERM_FLAG_UART);
    int mirror_fb = term && active && (term->flags & TERM_FLAG_FB) &&
                    display_get_active() == DISPLAY_TEXT_SESSION_ID;
    int mirror_remote = (pid >= 0 && owner == pid);

    if (term){
        unsigned int old_row = term->cursor_row;
        int dirty_all = terminal_char_will_scroll(term, c);
        dirty_start = old_row;
        dirty_end = old_row;
        terminal_buffer_putc_locked(term, c);
        if (dirty_all){
            dirty_start = 0u;
            dirty_end = (term->rows > 0u) ? (term->rows - 1u) : 0u;
        } else{
            terminal_dirty_note(&dirty_start, &dirty_end, term->cursor_row);
        }
    }
    do_render = mirror_fb;
    spin_unlock_irqrestore(&g_terminal_lock, irq);

    if (mirror_remote){
        remote_login_on_tty_output_char(c);
    }
    if (mirror_uart){
        if (c == '\n'){
            uart_send('\r');
        }
        uart_send(c);
    }
    if (do_render){
        terminal_render_rows_unlocked(term_id, dirty_start, dirty_end);
    }
}

void terminal_write(int term_id, int pid, const char* s, unsigned long len){
    if (!s){
        return;
    }

    int owner = (pid >= 0) ? console_get_owner() : -1;
    int do_render = 0;
    int do_uart = 0;
    int do_remote = 0;
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(term_id);
    int active = (term_id == g_active_term);
    int mirror_uart = !term || (term->flags & TERM_FLAG_UART);
    int mirror_fb = term && active && (term->flags & TERM_FLAG_FB) &&
                    display_get_active() == DISPLAY_TEXT_SESSION_ID;
    int mirror_remote = (pid >= 0 && owner == pid);
    unsigned int dirty_start = term ? term->cursor_row : 0u;
    unsigned int dirty_end = dirty_start;
    int dirty_all = 0;

    for (unsigned long i = 0; i < len; i++){
        char c = s[i];
        if (term){
            unsigned int old_row = term->cursor_row;
            if (terminal_char_will_scroll(term, c)){
                dirty_all = 1;
            }
            terminal_buffer_putc_locked(term, c);
            if (dirty_all){
                dirty_start = 0;
                dirty_end = (term->rows > 0u) ? (term->rows - 1u) : 0u;
            } else{
                terminal_dirty_note(&dirty_start, &dirty_end, old_row);
                terminal_dirty_note(&dirty_start, &dirty_end, term->cursor_row);
            }
        }
    }
    do_render = mirror_fb;
    do_uart = mirror_uart;
    do_remote = mirror_remote;
    spin_unlock_irqrestore(&g_terminal_lock, irq);

    if (do_remote){
        for (unsigned long i = 0; i < len; i++){
            remote_login_on_tty_output_char(s[i]);
        }
    }

    if (do_uart){
        for (unsigned long i = 0; i < len; i++){
            char c = s[i];
            if (c == '\n'){
                uart_send('\r');
            }
            uart_send(c);
        }
    }

    if (do_render){
        terminal_render_rows_unlocked(term_id, dirty_start, dirty_end);
    }
}

void terminal_poll_inputs(void){
    int term_id = -1;
    int pid = -1;
    int owner = console_get_owner();
    char c = 0;
    unsigned int src = 0;

    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    term_id = g_active_term;
    if (terminal_valid_id(term_id)){
        pid = g_terms[term_id].foreground_pid;
        if (owner >= 0 &&
            owner < TERM_PID_MAP_MAX &&
            g_pid_term[owner] == (signed char)term_id &&
            pid != owner){
            g_terms[term_id].foreground_pid = owner;
            pid = owner;
        }
    }
    spin_unlock_irqrestore(&g_terminal_lock, irq);

    if (pid < 0 || !terminal_valid_id(term_id)){
        return;
    }
    if (!console_try_getc_for_pid_ex(pid, &c, &src)){
        return;
    }

    irq = spin_lock_irqsave(&g_terminal_lock);
    if (terminal_pid_is_foreground_locked(term_id, pid)){
        (void)terminal_queue_push_locked(&g_terms[term_id], c, src);
    }
    spin_unlock_irqrestore(&g_terminal_lock, irq);
}

int terminal_read(int term_id, int pid, char* out, unsigned int* out_source){
    if (!terminal_valid_id(term_id) || !out){
        return 0;
    }

    terminal_poll_inputs();

    int owner = console_get_owner();
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(term_id);
    if (term &&
        pid >= 0 &&
        pid < TERM_PID_MAP_MAX &&
        owner == pid &&
        g_pid_term[pid] == (signed char)term_id &&
        term->foreground_pid != pid){
        term->foreground_pid = pid;
    }
    if (!term ||
        pid < 0 ||
        pid >= TERM_PID_MAP_MAX ||
        g_pid_term[pid] != (signed char)term_id ||
        term->foreground_pid != pid){
        spin_unlock_irqrestore(&g_terminal_lock, irq);
        return 0;
    }
    int ok = terminal_queue_pop_locked(term, out, out_source);
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return ok;
}

void terminal_putc_for_pid(int pid, char c){
    terminal_putc(terminal_get_for_pid(pid), pid, c);
}

void terminal_write_for_pid(int pid, const char* s, unsigned long len){
    terminal_write(terminal_get_for_pid(pid), pid, s, len);
}

int terminal_try_getc_for_pid(int pid, char* out){
    return terminal_read(terminal_get_for_pid(pid), pid, out, 0);
}

int terminal_try_getc_for_pid_ex(int pid, char* out, unsigned int* out_source){
    return terminal_read(terminal_get_for_pid(pid), pid, out, out_source);
}

int terminal_get_active(void){
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    int active = g_active_term;
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return active;
}

int terminal_set_active(int id){
    if (!terminal_valid_id(id)){
        return -1;
    }
    (void)display_set_active(DISPLAY_TEXT_SESSION_ID);
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    g_active_term = id;
    g_terms[g_active_term].flags |= TERM_FLAG_FB;
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    terminal_render_all_unlocked(id);
    return 0;
}

int terminal_switch_display_session(int id){
    if (id == DISPLAY_TEXT_SESSION_ID){
        return terminal_set_active(0);
    }

    display_session_t info;
    if (display_get_info(id, &info) != 0 || info.type != DISPLAY_GRAPHICS){
        return -1;
    }
    return display_set_active(id);
}

int terminal_cycle_display_session(int direction){
    int active = display_get_active();
    int dir = (direction < 0) ? -1 : 1;
    if (active < 0 || active >= DISPLAY_MAX_SESSIONS){
        active = DISPLAY_TEXT_SESSION_ID;
    }

    for (int i = 0; i < DISPLAY_MAX_SESSIONS; i++){
        int next = active + dir;
        if (next < 0){
            next = DISPLAY_MAX_SESSIONS - 1;
        } else if (next >= DISPLAY_MAX_SESSIONS){
            next = 0;
        }
        active = next;

        if (next == DISPLAY_TEXT_SESSION_ID){
            return terminal_switch_display_session(next);
        }

        display_session_t info;
        if (display_get_info(next, &info) == 0 && info.type == DISPLAY_GRAPHICS){
            return display_set_active(next);
        }
    }
    return -1;
}

unsigned int terminal_get_active_output(void){
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(g_active_term);
    unsigned int flags = term ? term->flags : 0u;
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return flags & (TERM_FLAG_UART | TERM_FLAG_FB);
}

int terminal_set_active_output(unsigned int flags){
    flags &= (TERM_FLAG_UART | TERM_FLAG_FB);
    if (flags == 0u){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(g_active_term);
    int term_id = g_active_term;
    if (!term){
        spin_unlock_irqrestore(&g_terminal_lock, irq);
        return -1;
    }
    term->flags = flags;
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    if (flags & TERM_FLAG_FB){
        (void)display_set_active(DISPLAY_TEXT_SESSION_ID);
        terminal_render_all_unlocked(term_id);
    }
    return 0;
}
