#ifndef LEANOS_TESTS_CHROMIUM_BINDGEN_LAYOUT_H
#define LEANOS_TESTS_CHROMIUM_BINDGEN_LAYOUT_H

#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>

struct bindgen_layout_sigset {
  sigset_t value;
};

#if defined(__lean_os__)
struct bindgen_layout_identity {
  char platform[1];
};
#else
struct bindgen_layout_identity {
  char platform[2];
};
#endif

struct bindgen_layout_scalars {
  long a_long;
  void *a_pointer;
  long double a_long_double;
  wchar_t a_wide_character;
};

#endif
