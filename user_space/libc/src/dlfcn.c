#include <dlfcn.h>
#include <errno.h>
#include <string.h>

/* dlopen, dlsym, dlclose and dlerror live in the dynamic loader
   (user_space/loader/ld-lean.c), because only a program the loader started
   has anything for them to answer about. dladdr does not live there, and
   this is it: for a statically linked program the honest answer is that no
   shared object contains the address, which is what returning zero says.

   Dynamic linking is a standing deferral in CLAUDE.md, and this is the
   shape of it here rather than a gap somebody forgot. The condition for a
   real dladdr is the loader keeping a module list a program can query -
   which is the same condition dl_iterate_phdr has had since M139. */
int dladdr(const void *address, Dl_info *info) {
    (void)address;
    if (info) {
        memset(info, 0, sizeof(*info));
    }
    return 0;
}
