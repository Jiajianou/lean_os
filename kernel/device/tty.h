#pragma once

#include <stdint.h>

#include "termios.h"

#define TTY_LINE_MAX 512
#define TTY_INBUF    2048

#define TTY_OUTBUF   4096

typedef struct tty {
    char line[TTY_LINE_MAX];
    uint32_t line_length;

    char inbuf[TTY_INBUF];
    uint32_t in_head;
    uint32_t in_tail;

    struct termios tio;

    int fg_pgid;
    int sid;

    uint16_t rows, cols;

    uint8_t is_pty;

    uint8_t hup;

    char outbuf[TTY_OUTBUF];
    uint32_t out_head;
    uint32_t out_tail;
} tty_t;

tty_t *tty_console(void);

void tty_init_pty(tty_t *t);

void tty_init(void);

void tty_input_char(tty_t *t, char c);

uint32_t tty_readable(const tty_t *t);

uint32_t tty_read(tty_t *t, char *buffer, uint32_t length);

int tty_may_read(tty_t *t, int sid, int pgid);


int tty_release_session(tty_t *t, int sid);

void tty_write(tty_t *t, const char *buffer, uint32_t length);
uint32_t tty_out_readable(const tty_t *t);
uint32_t tty_out_read(tty_t *t, char *buffer, uint32_t length);
