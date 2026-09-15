#pragma once

#include <elf.h>

#ifdef __cplusplus
extern "C" {
#endif

#define __ELF_NATIVE_CLASS 64

#define _ElfW(prefix, bits, type) _ElfW_1(prefix, bits, _##type)
#define _ElfW_1(prefix, bits, type) prefix##bits##type
#define ElfW(type) _ElfW(Elf, __ELF_NATIVE_CLASS, type)

#ifdef __cplusplus
}
#endif
