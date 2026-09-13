#pragma once

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[108];
};

#define SUN_LEN(p) ((size_t)(((struct sockaddr_un *)0)->sun_path) + strlen((p)->sun_path))

#ifdef __cplusplus
}
#endif
