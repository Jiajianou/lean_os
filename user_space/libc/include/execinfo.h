#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The calling thread's return addresses, innermost first, found by the
   same unwinder C++ exceptions use - the .eh_frame tables the compiler
   emits - rather than by trusting frame pointers code may not keep. */
int backtrace(void **buffer, int size);

/* One malloc'd block the caller frees once: an array of strings, each
   "name+0xoffset [0xaddress]" where dladdr knows the name and the bare
   address where it does not. */
char **backtrace_symbols(void *const *buffer, int size);

void backtrace_symbols_fd(void *const *buffer, int size, int fd);

#ifdef __cplusplus
}
#endif
