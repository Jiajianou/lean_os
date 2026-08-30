/* user_space/libc/include/langinfo.h - M80 groundwork
 *
 * `nl_langinfo`, which on a system with one locale has one interesting
 * answer: CODESET is "ANSI_X3.4-1968" - ASCII - because that is what
 * this machine's font, its filesystem and its terminal actually handle
 * (M39/M57 give one glyph per byte).
 *
 * Saying ASCII rather than UTF-8 is the whole point of the file. A
 * program told UTF-8 would encode above U+007F and produce bytes nothing
 * here can draw; told ASCII, it either stays in range or reports that it
 * cannot represent something, which is the truth.
 */
#pragma once

#define CODESET     0
#define D_T_FMT     1
#define D_FMT       2
#define T_FMT       3
#define AM_STR      4
#define PM_STR      5
#define RADIXCHAR   6
#define THOUSEP     7
#define YESEXPR     8
#define NOEXPR      9

typedef int nl_item;

char *nl_langinfo(nl_item item);
