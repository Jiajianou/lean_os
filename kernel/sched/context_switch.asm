; kernel/sched/context_switch.asm
;
; context_switch(uint64_t *old_rsp_out, uint64_t new_rsp): saves the
; callee-saved registers (the ones the System V AMD64 ABI requires a
; function to preserve across a call) onto the current stack, stashes the
; resulting RSP into *old_rsp_out, then switches RSP to new_rsp and pops a
; DIFFERENT task's callee-saved registers back off before returning.
;
; The `ret` at the end doesn't return to this function's caller in the
; usual sense - it pops whatever return address is sitting on the new
; stack, which is either:
;   - back into sched.c's schedule(), at the point right after ITS call to
;     context_switch(), if that task has run and been preempted before, or
;   - task_entry_trampoline (kernel/sched/sched.c), the first time a freshly
;     created task ever runs - sched.c fabricates the new stack to look
;     exactly like a task that's already "mid-context_switch" so this
;     routine never needs to know the difference.
;
; Everything else about that task's state (general-purpose registers,
; RIP, and - if it was preempted mid-interrupt - the CPU's own iretq
; frame) lives further up that same stack, saved by isr_common_stub
; (isr_asm.asm) exactly as it would for any interrupted task. This
; routine additionally owns the 6 callee-saved registers the ABI says a
; caller can otherwise assume survive a call, plus RFLAGS explicitly:
; every IDT gate here is a 64-bit *interrupt* gate (idt.c), which clears
; IF on entry, and only `iretq` restores it - not the plain `ret` this
; routine uses to hand off to a task that has never actually been
; interrupted before (see sched.c's fabricated initial stack). Without
; saving/restoring RFLAGS ourselves, a brand-new task would silently
; inherit IF=0 from "still logically inside the timer ISR" and run with
; interrupts permanently disabled - found by testing, not by inspection.

bits 64

global context_switch
context_switch:
    ; System V AMD64: rdi = old_rsp_out, rsi = new_rsp
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq
    ret
