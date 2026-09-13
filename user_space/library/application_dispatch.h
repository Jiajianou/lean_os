#pragma once

const char *application_dispatch_name(const char *path);

int application_dispatch_index(const char *path, const char *const *names, int count);
