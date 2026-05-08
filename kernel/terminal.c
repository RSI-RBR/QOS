#include "terminal.h"
#include "fb_console.h"
#include "uart.h"
#include "console.h"
#include "remote_login.h"
#include "spinlock.h"

#define TERM_FLAG_UART 1u
#define TERM_FLAG_FB   2u
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

static void terminal_render_locked(const terminal_t* term){
    if (!term || term->cols == 0u || term->rows == 0u){
        return;
    }
    fb_console_load_screen(&term->cells[0][0],
                           TERM_MAX_COLS,
                           TERM_MAX_ROWS,
                           term->cursor_col,
                           term->cursor_row);
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
    terminal_render_locked(&g_terms[0]);
}

void terminal_clear_active(void){
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(g_active_term);
    if (term){
        terminal_clear_buffer_locked(term);
        if (term->flags & TERM_FLAG_FB){
            terminal_render_locked(term);
        }
    }
    spin_unlock_irqrestore(&g_terminal_lock, irq);
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
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(term_id);
    int active = (term_id == g_active_term);
    int mirror_fb = term && active && (term->flags & TERM_FLAG_FB) && (pid < 0 || owner == pid);
    int mirror_remote = (pid >= 0 && owner == pid);

    if (term){
        terminal_buffer_putc_locked(term, c);
    }
    if (mirror_remote){
        remote_login_on_tty_output_char(c);
    }
    if (c == '\n'){
        uart_send('\r');
    }
    uart_send(c);
    if (mirror_fb){
        fb_console_putc(c);
    }

    spin_unlock_irqrestore(&g_terminal_lock, irq);
}

void terminal_write(int term_id, int pid, const char* s, unsigned long len){
    if (!s){
        return;
    }

    int owner = (pid >= 0) ? console_get_owner() : -1;
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(term_id);
    int active = (term_id == g_active_term);
    int mirror_fb = term && active && (term->flags & TERM_FLAG_FB) && (pid < 0 || owner == pid);
    int mirror_remote = (pid >= 0 && owner == pid);

    for (unsigned long i = 0; i < len; i++){
        char c = s[i];
        if (term){
            terminal_buffer_putc_locked(term, c);
        }
        if (mirror_remote){
            remote_login_on_tty_output_char(c);
        }
        if (c == '\n'){
            uart_send('\r');
        }
        uart_send(c);
    }
    if (mirror_fb){
        terminal_render_locked(term);
    }

    spin_unlock_irqrestore(&g_terminal_lock, irq);
}

void terminal_poll_inputs(void){
    int term_id = -1;
    int pid = -1;
    char c = 0;
    unsigned int src = 0;

    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    term_id = g_active_term;
    if (terminal_valid_id(term_id)){
        pid = g_terms[term_id].foreground_pid;
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

    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    terminal_t* term = terminal_get_locked(term_id);
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
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    g_active_term = id;
    g_terms[g_active_term].flags |= TERM_FLAG_FB;
    terminal_render_locked(&g_terms[g_active_term]);
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return 0;
}
