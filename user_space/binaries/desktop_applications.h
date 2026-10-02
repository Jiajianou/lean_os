#pragma once

int desktop_application_settings(int selftest);

int desktop_application_task_manager(int selftest);

int desktop_application_widgets(int selftest);

int desktop_application_wireless(int selftest);

int desktop_application_files(int selftest);

int desktop_application_wallpaper_selftest(int selftest);

extern const char *desktop_application_argument;

#include <stddef.h>
#include <stdint.h>

int desktop_application_make_desktop_picture(const char *image_path, const char *output_path, char *message,
                                             size_t capacity);

int desktop_application_use_wallpaper(uint32_t wallpaper);
