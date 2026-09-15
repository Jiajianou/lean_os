#include <uchar.h>

#include <errno.h>
#include <wchar.h>

/* The marker this file and nothing else uses. A completed UTF-8 decode
   leaves mbstate_t's total at zero and nothing in this libc ever sets it to
   0xFF, so that value can mean "a low surrogate is waiting in wc" - which
   keeps mbstate_t the size every program already built against this libc
   believes it is. Growing the struct would have been the obvious thing and
   would have needed every third-party archive in the sysroot rebuilt. */
#define SURROGATE_PENDING 0xFFu

static mbstate_t unnamed_state;

static void reset_state(mbstate_t *st) {
    st->wc = 0;
    st->owed = 0;
    st->total = 0;
}

size_t mbrtoc32(char32_t *destination, const char *source, size_t n,
                mbstate_t *ps) {
    wchar_t wc = 0;
    size_t r = mbrtowc(&wc, source, n, ps);
    if (destination && r != (size_t)-1 && r != (size_t)-2) {
        *destination = (char32_t)wc;
    }
    return r;
}

size_t c32rtomb(char *destination, char32_t c32, mbstate_t *ps) {
    return wcrtomb(destination, (wchar_t)c32, ps);
}

size_t mbrtoc16(char16_t *destination, const char *source, size_t n,
                mbstate_t *ps) {
    mbstate_t *st = ps ? ps : &unnamed_state;
    if (!source) {
        reset_state(st);
        return 0;
    }
    if (st->total == SURROGATE_PENDING) {
        if (destination) {
            *destination = (char16_t)st->wc;
        }
        reset_state(st);
        /* Produced without consuming: the standard's own value for it. */
        return (size_t)-3;
    }
    wchar_t wc = 0;
    size_t r = mbrtowc(&wc, source, n, st);
    if (r == (size_t)-1 || r == (size_t)-2) {
        return r;
    }
    if ((uint_least32_t)wc >= 0x10000u) {
        uint_least32_t above = (uint_least32_t)wc - 0x10000u;
        if (destination) {
            *destination = (char16_t)(0xD800u + (above >> 10));
        }
        st->wc = (unsigned int)(0xDC00u + (above & 0x3FFu));
        st->owed = 0;
        st->total = SURROGATE_PENDING;
        return r;
    }
    if (destination) {
        *destination = (char16_t)wc;
    }
    return r;
}

size_t c16rtomb(char *destination, char16_t c16, mbstate_t *ps) {
    mbstate_t *st = ps ? ps : &unnamed_state;
    if (!destination) {
        reset_state(st);
        return 1;
    }
    if (st->total == SURROGATE_PENDING) {
        unsigned int high = st->wc;
        reset_state(st);
        if (c16 < 0xDC00u || c16 > 0xDFFFu) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        wchar_t wc = (wchar_t)(0x10000u + ((high - 0xD800u) << 10) +
                               (c16 - 0xDC00u));
        return wcrtomb(destination, wc, (mbstate_t *)0);
    }
    if (c16 >= 0xD800u && c16 <= 0xDBFFu) {
        st->wc = (unsigned int)c16;
        st->owed = 0;
        st->total = SURROGATE_PENDING;
        /* Nothing written yet: the low half decides what the bytes are. */
        return 0;
    }
    if (c16 >= 0xDC00u && c16 <= 0xDFFFu) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    return wcrtomb(destination, (wchar_t)c16, (mbstate_t *)0);
}
