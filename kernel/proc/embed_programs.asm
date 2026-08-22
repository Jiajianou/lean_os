; kernel/proc/embed_programs.asm
;
; Embeds every user program this project ships (see the Makefile's
; USER_PROGRAMS list) as byte blobs inside the kernel image, since
; there's no way to get them onto the disk filesystem (kernel/fs/
; leanfs.c) other than the kernel seeding them there itself on first
; boot - stage2's bootloader has no filesystem driver of its own, and
; nothing outside this kernel has ever written to the disk. incbin's
; paths are relative to the Makefile's working directory (repo root),
; matching every other build-relative path in this project.
;
; Written out longhand rather than via a macro over a name list: nasm
; doesn't substitute macro parameters inside a double-quoted string
; literal, so a parameterized `incbin "build/%1.elf"` doesn't work.
;
; The Makefile adds an explicit extra prerequisite (every build/*.elf
; this file incbins) on this object's target, on top of the normal
; *.asm pattern rule - incbin needs each of those files to exist before
; nasm can even assemble this one.

section .rodata

global hello_elf_start
global hello_elf_end
hello_elf_start:
    incbin "build/hello.elf"
hello_elf_end:

global echo_elf_start
global echo_elf_end
echo_elf_start:
    incbin "build/echo.elf"
echo_elf_end:

global cat_elf_start
global cat_elf_end
cat_elf_start:
    incbin "build/cat.elf"
cat_elf_end:

global ls_elf_start
global ls_elf_end
ls_elf_start:
    incbin "build/ls.elf"
ls_elf_end:

global init_elf_start
global init_elf_end
init_elf_start:
    incbin "build/init.elf"
init_elf_end:

global shell_elf_start
global shell_elf_end
shell_elf_start:
    incbin "build/shell.elf"
shell_elf_end:

global memtest_elf_start
global memtest_elf_end
memtest_elf_start:
    incbin "build/memtest.elf"
memtest_elf_end:

global compositor_elf_start
global compositor_elf_end
compositor_elf_start:
    incbin "build/compositor.elf"
compositor_elf_end:

global wm_demo_elf_start
global wm_demo_elf_end
wm_demo_elf_start:
    incbin "build/wm_demo.elf"
wm_demo_elf_end:

global gui_clock_elf_start
global gui_clock_elf_end
gui_clock_elf_start:
    incbin "build/gui_clock.elf"
gui_clock_elf_end:

global gui_paint_elf_start
global gui_paint_elf_end
gui_paint_elf_start:
    incbin "build/gui_paint.elf"
gui_paint_elf_end:

global desktop_shell_elf_start
global desktop_shell_elf_end
desktop_shell_elf_start:
    incbin "build/desktop_shell.elf"
desktop_shell_elf_end:
