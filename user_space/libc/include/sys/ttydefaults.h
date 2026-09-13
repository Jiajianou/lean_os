#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CTRL(x) ((x) & 0x1F)

#define CEOF     CTRL('d')
#define CERASE   0x7F
#define CINTR    CTRL('c')
#define CKILL    CTRL('u')
#define CQUIT    CTRL('\\')
#define CSUSP    CTRL('z')
#define CSTART   CTRL('q')
#define CSTOP    CTRL('s')
#define CEOL     0
#define CMIN     1
#define CTIME    0
#define CWERASE  CTRL('w')
#define CLNEXT   CTRL('v')
#define CREPRINT CTRL('r')
#define CDISCARD CTRL('o')
#define CEOT     CEOF
#define CBRK     CEOL
#define CRPRNT   CREPRINT
#define CFLUSH   CDISCARD

#ifdef __cplusplus
}
#endif
