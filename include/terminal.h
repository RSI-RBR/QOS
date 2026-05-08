#ifndef TERMINAL_H
#define TERMINAL_H

#define QOS_TERMINAL_MAX 4

void terminal_init(void);
void terminal_clear_active(void);
void terminal_putc_for_pid(int pid, char c);
void terminal_write_for_pid(int pid, const char* s, unsigned long len);
int terminal_try_getc_for_pid(int pid, char* out);
int terminal_try_getc_for_pid_ex(int pid, char* out, unsigned int* out_source);
int terminal_get_active(void);
int terminal_set_active(int id);

#endif
