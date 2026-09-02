; user_space/lib/crtn.asm - M94
;
; The second half of _init and _fini - see crti.asm, which opens both and
; explains what they are for. This closes them, and must be the LAST
; object on the link line for the same reason crti.o must be nearly the
; first: the two halves are a function only because of where they sit.

bits 64

section .init
    pop rbp
    ret

section .fini
    pop rbp
    ret
