/* tests/fakes/errno.h - M88. See tests/fakes/wchar.h for why this
 * redirect exists; this is its second half. The conversions under test
 * report a malformed sequence by setting EILSEQ, and EILSEQ is a number
 * this project chose (to match Linux's) rather than one the host's
 * <errno.h> would have agreed with by luck. */
#pragma once
#include "../../user_space/libc/include/errno.h"
