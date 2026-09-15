#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NL_SETD 1
#define NL_CAT_LOCALE 1

typedef void *nl_catd;
typedef int nl_item;

/* Message catalogues, which this OS has none of: there is no catalogue
   format, no catopen search path and no tool that produces one. catopen
   says so - it returns (nl_catd)-1, which is the answer POSIX gives for a
   catalogue that is not there - and catgets then returns the string the
   caller passed in, which is the behaviour every program using these is
   already written to handle, because a missing catalogue is the ordinary
   case rather than an error.

   libc++'s std::messages is what asks for this header, and it degrades to
   exactly that. The condition for real catalogues is a program on this
   machine that ships one. */
nl_catd catopen(const char *name, int flag);
char *catgets(nl_catd catalogue, int set, int message, const char *fallback);
int catclose(nl_catd catalogue);

#ifdef __cplusplus
}
#endif
