#pragma once

#include "paths.h"

#define RECENT_PATH  PATH_ETC_DIRECTORY "recent.conf"
#define RECENT_MAX   8

void recent_add(const char *path);

int recent_load(char out[][PATH_MAX_LENGTH], int max);
