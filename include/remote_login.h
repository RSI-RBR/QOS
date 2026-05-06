#ifndef REMOTE_LOGIN_H
#define REMOTE_LOGIN_H

int remote_login_init(void);
void remote_login_poll(void);
int remote_login_enabled(void);
void remote_login_dump_stats(void);

#endif
