#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CODESET       0
#define D_T_FMT       1
#define D_FMT         2
#define T_FMT         3
#define T_FMT_AMPM    4
#define AM_STR        5
#define PM_STR        6

#define DAY_1         10
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

#define MON_1         30
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
