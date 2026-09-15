#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL  0x0000

/* The two pseudo-handles. RTLD_NEXT is how a program interposes a function
   and still reaches the one it replaced. */
#define RTLD_NEXT    ((void *)-1L)
#define RTLD_DEFAULT ((void *)0)

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

void *dlopen(const char *file, int flags);
void *dlsym(void *handle, const char *name);
int   dlclose(void *handle);
char *dlerror(void);
int   dladdr(const void *address, Dl_info *info);

#ifdef __cplusplus
}
#endif
