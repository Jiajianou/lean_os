#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_write      0
#define SYS_exit       1
#define SYS_getpid     2
#define SPAWN_MAX_ARGUMENTS 256

#define SYS_spawn      3
#define SYS_wait       4
#define SYS_read       5
#define SYS_readfile   6
#define SYS_listdir    7
#define SYS_kill       8
#define SYS_pipe       9
#define SYS_getpgid   10
#define SYS_sbrk      11
#define SYS_shared_memory_create 12
#define SYS_shared_memory_map   13
#define SYS_framebuffer_info   14
#define SYS_framebuffer_map    15
#define SYS_mouse_read 16
#define SYS_pipe_open 17
#define SYS_keyboard_read  18
#define SYS_pipe_poll 19
#define SYS_uptime_ms 20
#define SYS_dup2      21
#define SYS_wait_nb   22
#define SYS_yield     23
#define SYS_task_alive 24
#define SYS_pipe_reset 25

#define SYS_keyboard_modifiers 26
#define SYS_clipboard_set 27
#define SYS_clipboard_get 28
#define SYS_writefile 29

#define SYS_taskinfo 30

#define SYS_shutdown 31

#define SYS_close 32

#define SYS_shared_memory_free 33

#define SYS_mkdir 34

#define SYS_shared_memory_unmap 35

#define SYS_unlink 36
#define SYS_rename 37

#define OPEN_READ     0x1
#define OPEN_WRITE    0x2
#define OPEN_CREATE   0x4
#define OPEN_TRUNCATE 0x8
#define OPEN_APPEND   0x10
#define OPEN_CLOEXEC  0x20
#define OPEN_EXCL     0x40
#define OPEN_NOFOLLOW 0x80
#define OPEN_SYNC     0x100

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define SYS_open   40
#define SYS_lseek  41
#define SYS_stat   42

#define OS_ERROR_NOENT 2
#define OS_ERROR_FAULT 14
#define OS_ERROR_ACCESS 13
#define OS_ERROR_INTR  4
#define OS_ERROR_AGAIN 5
#define OS_ERROR_PERMISSION 1
#define OS_ERROR_INVALID 22
#define OS_ERROR_NFILE 23
#define OS_ERROR_MFILE 24
#define SYS_rmdir  43
#define SYS_time   44

#define SYS_audio_claim  45
#define SYS_beep         46
#define SYS_audio_volume 47
#define SYS_audio_release 49
#define SYS_audio_play   48

#define SYS_display_modes 38
#define SYS_display_set_mode 39

#define SYS_socket   50
#define SYS_bind     51
#define SYS_sendto   52
#define SYS_recvfrom 53
#define SYS_sockpoll 54
#define SYS_netconf  55
#define SYS_settime  56

#define SYS_listen   59
#define SYS_connect  60
#define SYS_connstat 61
#define SYS_accept   62
#define SYS_send     63
#define SYS_receive     64

#define SYS_getcaps  57
#define SYS_dropcaps 58

#define SYS_PIPE_CAPACITY 1024

#define SYS_kernel_log 65
#define SYS_kernel_log_total 66

#define SYS_rename_replace 67

#define SYS_waitfds 68
#define SYS_idle_ticks 69

#define SYS_chdir 70
#define SYS_getcwd 71

#define SYS_sigaction 72
#define SYS_sigreturn 73
#define SYS_sigprocmask 74

#define SYS_fstat 75

#define SYS_mmap   76
#define SYS_munmap 77

#define SYS_thread_create 78
#define SYS_thread_exit 79
#define SYS_gettid 80

#define SYS_getdents   81

#define SYS_fork       82

#define SYS_execve     83

#define WNOHANG 1
#define WUNTRACED 2
#define SYS_waitpid    84

#define F_GETFD_COMMAND 1
#define F_SETFD_COMMAND 2
#define F_GETFL_COMMAND 3
#define FILE_DESCRIPTOR_CLOEXEC_BIT 1
#define F_GETLK_COMMAND  4
#define F_SETLK_COMMAND  5
#define F_SETLKW_COMMAND 6
#define F_SETFL_COMMAND  7
/* F_DUPFD in the kernel rather than in the library: the lowest free slot at or
   above the argument, found and filled in one step. The library used to find
   one and then dup2 onto it, which a second thread opening a descriptor in
   between turned into a silent close of that thread's new descriptor - and it
   only ever looked at the first 128 slots (M187). */
#define F_DUPFD_COMMAND         8
#define F_DUPFD_CLOEXEC_COMMAND 9
#define OS_NONBLOCK_BIT 0x800
#define SYS_fcntl      85

#define SYS_setpgid    86
#define SYS_setsid     87
#define SYS_getsid     88

#define SYS_ioctl      89

#define SYS_ftruncate  90

#define SYS_symlink    91
#define SYS_readlink   92
#define SYS_lstat      93

#define OS_NAME_MAX 255
#define OS_DT_UNKNOWN 0
#define OS_DT_DIRECTORY     4
#define OS_DT_REG     8
#define OS_DT_LNK     10

typedef struct {
    unsigned int   ino;
    unsigned short reclen;
    unsigned char  type;
    unsigned char  name_length;
    char           name[];
} os_dirent_t;

#define OS_DIRENT_MAX (8 + OS_NAME_MAX + 1 + 7)

#define SYS_mprotect   94
#define SYS_fsync      97

#define SYS_link       96

#define SYS_madvise    95

#define SYS_profile    98

#define SYS_rusage 99

#define SYS_statvfs 100

#define SYS_utime 101

#define SYS_fdpath 102

#define SYS_getppid 103

#define SYS_sync 104

#define SYS_meminfo 105

#define SYS_alarm 106

#define SYS_msync 107

#define SYS_arch_prctl 108

#define SYS_futex 109

#define SYS_getrandom 110
#define GRND_NONBLOCK_BIT 1
#define GRND_RANDOM_BIT   2

#define SYS_pread  111
#define SYS_pwrite 112

#define OS_ERROR_SPIPE 29
#define OS_ERROR_SEARCH 3
/* dup2() onto a descriptor another thread has numbered and not yet filled. */
#define OS_ERROR_BUSY 16

#define SYS_socketpair 113
#define SYS_bindun     114
#define SYS_connectun  115
#define SYS_sendmsg    116
#define SYS_recvmsg    117

#define SYS_sockshut   118

#define SYS_epoll_create    119
#define SYS_epoll_control       120
#define SYS_epoll_wait      121
#define SYS_eventfd         122
#define SYS_timerfd_create  123
#define SYS_timerfd_settime 124
#define SYS_timerfd_gettime 125

#define SYS_memfd_create 126
#define SYS_memfd_seal   127

#define SYS_unix_peer_credentials 128
#define SYS_sigaltstack           129
#define SYS_mincore               130
#define SYS_sockname              131

/* A thread's own name, which is what pthread_setname_np(3) sets. It goes
   where the scheduler already keeps one - task_t.name, the field /bin/ps and
   the task manager read - rather than into a field only the C library can
   see, because a name nothing can read would be the pretence M65 refuses.
   TASK_NAME_MAX is 24; Linux's own limit for the same call is 16. */
#define SYS_thread_setname        132
#define SYS_thread_getname        133

/* Resource limits. A C library cannot answer these honestly: getrlimit has
   to say EFAULT for a pointer it cannot write, and only the kernel can look
   at a page table before writing. See system_api/include/os_resource.h. */
#define SYS_getrlimit             134
#define SYS_setrlimit             135

/* recv(2)'s MSG_PEEK: what is in a socket's receive queue, without taking
   it out. A C library cannot answer this by reading and remembering - the
   bytes are gone from the kernel's queue the moment it reads them, and a
   second descriptor for the same socket would not see them. See M183: a
   peek that consumed its byte cost this machine every https page. */
#define SYS_peek                  136

/* M200: nanoseconds since boot, from the TSC - what CLOCK_MONOTONIC means.
   SYS_uptime_ms is the same clock cut to a millisecond. */
#define SYS_clock_ns              137

/* M205: pthread_detach(3). The thread's slot in the kernel's task table goes
   back once it has terminated, because nobody will ever wait for it - which
   until this call only the C library knew. */
#define SYS_thread_detach         138

/* M207: the wireless network - see system_api/include/wireless.h. One call,
   an operation and two arguments, so the next operation is not a new number. */
#define SYS_wireless              139

/* M209: how a process that is not your child ended, as a wait(2) status -
   which the compositor needs to tell a crash from a program that quit. */
#define SYS_task_end_status       140

/* M213: the display's scale and the mode the next boot starts in - see
   system_api/include/display.h. */
#define SYS_display               141
#define SYSCALL_COUNT 142

#ifdef __cplusplus
}
#endif
