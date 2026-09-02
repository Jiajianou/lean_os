/* user_space/libc/include/sys/utsname.h - M89
 *
 * `uname`, which is how a program asks what it is running on. Every
 * field is answered truthfully and none of them pretends to be Linux -
 * a program that switches on sysname and does not recognise this one
 * should take its portable path, which is the behaviour that gets a
 * ported program working rather than subtly wrong.
 */
#pragma once

#define _UTSNAME_LENGTH 65

struct utsname {
    char sysname[_UTSNAME_LENGTH];  /* "lean_os" */
    char nodename[_UTSNAME_LENGTH]; /* this machine has no name it was given - see uname.c */
    char release[_UTSNAME_LENGTH];  /* the milestone number this kernel was built at */
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];  /* "x86_64" - the only architecture there is */
    char domainname[_UTSNAME_LENGTH];
};

int uname(struct utsname *buf);
