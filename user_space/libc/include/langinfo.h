/* user_space/libc/include/langinfo.h - M80 groundwork, M88 correction
 *
 * `nl_langinfo`, which on a system with one locale has one interesting
 * answer: CODESET.
 *
 * **It said ASCII until M88, on purpose, and now says UTF-8 - also on
 * purpose.** The old note argued the case exactly right for the system
 * that existed then: "a program told UTF-8 would encode above U+007F and
 * produce bytes nothing here can draw; told ASCII, it either stays in
 * range or reports that it cannot represent something, which is the
 * truth." What changed underneath it is that the conversions in
 * <wchar.h> are now real UTF-8 and the whole system is byte-transparent,
 * so a program that encodes above U+007F now produces bytes that
 * round-trip through this filesystem, this terminal and this clipboard
 * unharmed. They still DRAW as a replacement box above U+00FF, which is
 * a fact about the font rather than about the encoding - and telling a
 * program the codeset is ASCII to warn it about a font is answering a
 * question it did not ask.
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
