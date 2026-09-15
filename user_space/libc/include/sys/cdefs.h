#pragma once

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS   }
/* noexcept is C++11's spelling and throw() is C++98's, and this has to be
   the one the translation unit can actually parse: libstdc++ builds its own
   src/c++98 with -std=gnu++98, where `noexcept` is not a keyword and a
   declaration carrying it does not compile. */
#if __cplusplus >= 201103L
#define __THROW       noexcept
#define __THROWNL     noexcept
#define __NTH(fct)    fct noexcept
#define __NTHNL(fct)  fct noexcept
#else
#define __THROW       throw()
#define __THROWNL     throw()
#define __NTH(fct)    fct throw()
#define __NTHNL(fct)  fct throw()
#endif
#else
#define __BEGIN_DECLS
#define __END_DECLS
#define __THROW
#define __THROWNL
#define __NTH(fct)    fct
#define __NTHNL(fct)  fct
#endif

#define __CONCAT(a, b) a##b
#define __STRING(x)    #x

#define __P(args) args
