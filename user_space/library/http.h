#pragma once

#include <stdint.h>

#define HTTP_MAX_REDIRECTS 3

long http_get(const char *url, char *body, long cap, int *status_out);
