#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int *__errno_location(void);
#define errno (*__errno_location())

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENXIO   6
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define ENOTBLK 15
#define EBUSY   16
#define EEXIST  17
#define EXDEV   18
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define ETXTBSY 26
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EMLINK  31
#define EPIPE   32
#define EDOM    33
#define ERANGE  34
#define ENAMETOOLONG 36
#define ENOSYS  38
#define ENOTEMPTY 39
#define ELOOP   40
#define ENOMEDIUM 123
#define ENODATA 61
#define EILSEQ  84
#define ENOTSOCK 88
#define EOPNOTSUPP 95
#define EADDRINUSE 98
#define ECONNREFUSED 111
#define ETIMEDOUT 110
#define EINPROGRESS 115
#define EALREADY 114
#define ECONNRESET 104
#define ECONNABORTED 103
#define ENOTCONN        107
#define EAFNOSUPPORT    97
#define ESOCKTNOSUPPORT 94
#define EPROTONOSUPPORT 93
#define ENOPROTOOPT     92
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define ENETUNREACH     101
#define ENETDOWN        100
#define ENOTSUP EOPNOTSUPP
#define EHOSTUNREACH 113

#define EDEADLK      35
#define ENOLCK       37
#define EADDRNOTAVAIL 99
#define ENOMSG       42
#define EIDRM        43
#define ENOLINK      67
#define EPROTO       71
#define EMULTIHOP    72
#define EBADMSG      74
#define EOVERFLOW    75
#define ENOTUNIQ     76
#define EPROTOTYPE   91
#define ENETRESET    102
#define ENOBUFS      105
#define EISCONN      106
#define ETOOMANYREFS 109
#define ESTALE       116
#define EDQUOT       122
#define ECANCELED    125
#define EOWNERDEAD   130
#define ENOTRECOVERABLE 131
#define EWOULDBLOCK EAGAIN

/* The rest of the socket errors, at the numbers Linux gives them - which
   is what a program that stores an errno in a file or sends it over a
   wire expects to read back. Nothing here RETURNS these yet; they exist
   because somebody else's error table names them. */
#define EUSERS       87
#define ESHUTDOWN    108
#define EHOSTDOWN    112
#define EPFNOSUPPORT 96
#define EREMOTE      66
#define ENOPKG       65
#define ENONET       64

#ifdef __cplusplus
}
#endif
