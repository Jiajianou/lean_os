#pragma once

#include <stdint.h>

#include "tty.h"

#define PTY_MAX 8

int pty_alloc(void);

tty_t *pty_tty(int n);

int pty_valid(int n);

void pty_slave_opened(int n);
void pty_slave_closed(int n);
void pty_master_closed(int n);

int64_t pty_master_read(int n, char *buffer, uint32_t length);
int64_t pty_master_write(int n, const char *buffer, uint32_t length);
int64_t pty_slave_read(int n, char *buffer, uint32_t length);
int64_t pty_slave_write(int n, const char *buffer, uint32_t length);

int pty_master_readable(int n);
int pty_slave_readable(int n);

void pty_release_session(int sid);
