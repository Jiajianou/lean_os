#pragma once

#include "paths.h"
#include "window_manager.h"

#define SETTINGS_FILE_NAME PATH_SETTINGS

void settings_file_defaults(window_manager_settings_request_t *out);

int settings_file_load(window_manager_settings_request_t *out);

int settings_file_load_display(uint32_t *w, uint32_t *h);

int settings_file_save_display(uint32_t w, uint32_t h);

int settings_file_load_display_scale(uint32_t *percent);

int settings_file_save_display_scale(uint32_t percent);

int settings_file_save(const window_manager_settings_request_t *in);
