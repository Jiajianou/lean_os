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

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

/* ---- M99: the whole set, because "should exist" is not a guard -------
 *
 * M88 left this at ten items, which was the list that had been asked
 * for. CPython's Modules/_localemodule.c asks for fifty-odd, and the
 * first thirty-eight of them - the day and month names - are under a
 * comment that reads "These constants should exist on any langinfo
 * implementation" and under NO #ifdef. That is a reasonable thing for it
 * to assume and this header was the thing that was wrong.
 *
 * The values are this library's own; nothing outside it persists them,
 * and POSIX specifies the names rather than the numbers. Grouped so the
 * arithmetic in a caller that writes `MON_1 + m` works, which is what
 * the numbering is FOR - `nl_langinfo(DAY_1 + tm_wday)` is how these are
 * used everywhere.
 */
#define CODESET       0
#define D_T_FMT       1
#define D_FMT         2
#define T_FMT         3
#define T_FMT_AMPM    4
#define AM_STR        5
#define PM_STR        6

#define DAY_1         10 /* Sunday, and the run is consecutive */
#define DAY_2         11
#define DAY_3         12
#define DAY_4         13
#define DAY_5         14
#define DAY_6         15
#define DAY_7         16

#define ABDAY_1       20
#define ABDAY_2       21
#define ABDAY_3       22
#define ABDAY_4       23
#define ABDAY_5       24
#define ABDAY_6       25
#define ABDAY_7       26

#define MON_1         30 /* January, and the run is consecutive */
#define MON_2         31
#define MON_3         32
#define MON_4         33
#define MON_5         34
#define MON_6         35
#define MON_7         36
#define MON_8         37
#define MON_9         38
#define MON_10        39
#define MON_11        40
#define MON_12        41

#define ABMON_1       50
#define ABMON_2       51
#define ABMON_3       52
#define ABMON_4       53
#define ABMON_5       54
#define ABMON_6       55
#define ABMON_7       56
#define ABMON_8       57
#define ABMON_9       58
#define ABMON_10      59
#define ABMON_11      60
#define ABMON_12      61

#define RADIXCHAR     70
#define THOUSEP       71
#define YESEXPR       72
#define NOEXPR        73
#define CRNCYSTR      74

/* The era items. Defined so that a program which asks gets the C
 * locale's answer - which is the empty string, because the C locale has
 * no alternative era and saying so is not the same as not answering. */
#define ERA           80
#define ERA_D_FMT     81
#define ERA_D_T_FMT   82
#define ERA_T_FMT     83
#define ALT_DIGITS    84

typedef int nl_item;

char *nl_langinfo(nl_item item);

#ifdef __cplusplus
}
#endif
