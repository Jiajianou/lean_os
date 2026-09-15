#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_INFO_NAME_MAX 24

#define TASK_INFO_MAX 128

#define TASK_INFO_READY      0
#define TASK_INFO_RUNNING    1
#define TASK_INFO_TERMINATED 2
#define TASK_INFO_BLOCKED    3

typedef struct {
    int32_t pid;
    int32_t parent_pid;
    int32_t pgid;
    int32_t state;
    int32_t exit_code;
    int32_t open_file_descriptors;
    int32_t shared_memory_segments;
    char name[TASK_INFO_NAME_MAX];
} task_info_t;

#define OS_RUSAGE_SELF     0
#define OS_RUSAGE_CHILDREN 1
#define OS_RUSAGE_THREAD   2

typedef struct {
    uint64_t user_ticks;
    uint64_t sys_ticks;
    uint64_t max_rss_pages;
} os_rusage_t;

typedef struct {
    uint64_t total_frames;
    uint64_t free_frames;
    uint64_t page_size;
} os_meminfo_t;

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9

#define USER_INTERP_BASE 0x0000008800000000ULL

#define USER_INTERP_PATH "/lib/ld-lean.so"

#ifdef __cplusplus
}
#endif
