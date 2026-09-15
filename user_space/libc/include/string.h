#pragma once

#include <stddef.h>
#include <strings.h>

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);

char *strerror(int errnum);
int strerror_r(int errnum, char *buf, size_t buflen);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);

char *strtok(char *s, const char *delim);
char *strtok_r(char *s, const char *delim, char **saveptr);
int strcoll(const char *a, const char *b);
size_t strxfrm(char *dst, const char *src, size_t n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *stpcpy(char *dst, const char *src);
char *stpncpy(char *dst, const char *src, size_t n);
void *memmem(const void *haystack, size_t hlen, const void *needle, size_t nlen);
void *memccpy(void *dst, const void *src, int c, size_t n);
size_t strnlen(const char *s, size_t n);


/* POSIX 2008's locale-argument forms. See
   user_space/libc/src/locale_functions.c for why they are what they are on
   a machine with exactly one locale - and for what they do NOT do, which is
   accept a locale this machine never made. */
int strcoll_l(const char *a, const char *b, locale_t locale);
size_t strxfrm_l(char *out, const char *in, size_t length, locale_t locale);

#ifdef __cplusplus
}
#endif
