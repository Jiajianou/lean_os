#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define IFNAMSIZ 16
#define IF_NAMESIZE IFNAMSIZ

struct if_nameindex {
    unsigned int if_index;
    char        *if_name;
};

unsigned int if_nametoindex(const char *name);
char        *if_indextoname(unsigned int index, char *name);

#ifdef __cplusplus
}
#endif
