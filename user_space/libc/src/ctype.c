/* user_space/libc/src/ctype.c - M111
 *
 * The out-of-line copies of <ctype.h>'s classifications, so that each of
 * them is a SYMBOL in libc.a and not only a definition in a header.
 *
 * Why that matters is in ctype.h's own note, and it is worth the two
 * lines here: a configure script does not include a header, it links -
 * so a function that exists only as a `static inline` reads as absent,
 * and gnulib then supplies its own copy, which collides with the one it
 * could not see. GNU grep 3.11 stopped on exactly that.
 *
 * The bodies are not repeated. LEANOS_CTYPE_OUT_OF_LINE makes ctype.h
 * expand its own list without `static inline`, so these sixteen
 * functions and the sixteen inline ones a caller gets are the same
 * sixteen expressions - which is the property that matters, because two
 * copies of `tolower` in one system is how they come to disagree.
 */
#define LEANOS_CTYPE_OUT_OF_LINE 1
#include <ctype.h>
