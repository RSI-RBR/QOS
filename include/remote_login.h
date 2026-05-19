#ifndef REMOTE_LOGIN_H
#define REMOTE_LOGIN_H

int remote_login_init(void);
void remote_login_poll(void);
int remote_login_enabled(void);
void remote_login_dump_stats(void);
int remote_login_try_read_tty_char(char* out);
void remote_login_on_tty_output_char(char c);
unsigned int remote_login_state_bits(void);
void remote_login_get_diag(unsigned long* rx,
                           unsigned long* tx,
                           unsigned long* bad_header,
                           unsigned long* bad_crypto);

#endif
