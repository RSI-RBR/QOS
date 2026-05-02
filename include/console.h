#ifndef CONSOLE_H
#define CONSOLE_H

void console_init(void);
int console_try_getc_for_pid(int pid, char* out);
int console_set_owner(int requester_pid, int target_pid);
int console_release_owner(int requester_pid);
int console_get_owner(void);
void console_owner_on_process_exit(int pid);

#endif
