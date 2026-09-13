#pragma once

#include <sys/resource.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG 1

#define WUNTRACED 2

#define WIFEXITED(status)   (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSTOPPED(status)  (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status)    (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0 && !WIFSTOPPED(status))
#define WTERMSIG(status)    ((status) & 0x7F)

pid_t waitpid(pid_t pid, int *status, int options);

pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage);
pid_t wait3(int *status, int options, struct rusage *usage);

pid_t wait(int *status);

#ifdef __cplusplus
}
#endif
