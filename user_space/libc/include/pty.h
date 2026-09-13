#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);
int ptsname_r(int fd, char *buf, size_t len);

int openpty(int *primary, int *secondary, char *name,
            const struct termios *tio, const struct winsize *ws);
pid_t forkpty(int *primary, char *name,
              const struct termios *tio, const struct winsize *ws);
int login_tty(int fd);

#ifdef __cplusplus
}
#endif
