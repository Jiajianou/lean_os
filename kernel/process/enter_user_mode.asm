bits 64

global enter_user_mode
enter_user_mode:
    mov ax, cx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push rcx
    push rsi
    pushfq
    pop rax
    or rax, 0x200
    push rax
    push r8
    push rdi
    mov rdi, rdx
    iretq
