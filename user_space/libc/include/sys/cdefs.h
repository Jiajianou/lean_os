#pragma once

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS   }
#define __THROW       noexcept
#define __THROWNL     noexcept
#define __NTH(fct)    fct noexcept
#define __NTHNL(fct)  fct noexcept
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
