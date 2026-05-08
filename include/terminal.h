#ifndef TERMINAL_H
#define TERMINAL_H

#define QOS_TERMINAL_MAX 4
#ifndef QOS_TERM_OUTPUT_UART
#define QOS_TERM_OUTPUT_UART 1u
#endif
#ifndef QOS_TERM_OUTPUT_FB
#define QOS_TERM_OUTPUT_FB   2u
#endif

void terminal_init(void);
void terminal_clear_active(void);
int terminal_attach_pid(int pid, int term_id);
void terminal_detach_pid(int pid);
int terminal_get_for_pid(int pid);
int terminal_set_foreground_pid(int term_id, int pid);
int terminal_get_foreground_pid(int term_id);
void terminal_putc(int term_id, int pid, char c);
void terminal_write(int term_id, int pid, const char* s, unsigned long len);
void terminal_poll_inputs(void);
int terminal_read(int term_id, int pid, char* out, unsigned int* out_source);
void terminal_putc_for_pid(int pid, char c);
void terminal_write_for_pid(int pid, const char* s, unsigned long len);
int terminal_try_getc_for_pid(int pid, char* out);
int terminal_try_getc_for_pid_ex(int pid, char* out, unsigned int* out_source);
int terminal_get_active(void);
int terminal_set_active(int id);
unsigned int terminal_get_active_output(void);
int terminal_set_active_output(unsigned int flags);

#endif
