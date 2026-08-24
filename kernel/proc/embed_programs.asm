; kernel/proc/embed_programs.asm
;
; Embeds every user program this project ships (see the Makefile's
; USER_PROGRAMS list) as byte blobs inside the kernel image, since
; there's no way to get them onto the disk filesystem (kernel/fs/
; leanfs.c) other than the kernel seeding them there itself on first
; boot - the boot loader has no filesystem driver of its own, and
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

global desktop_icons_elf_start
global desktop_icons_elf_end
desktop_icons_elf_start:
    incbin "build/desktop_icons.elf"
desktop_icons_elf_end:

global gui_terminal_elf_start
global gui_terminal_elf_end
gui_terminal_elf_start:
    incbin "build/gui_terminal.elf"
gui_terminal_elf_end:

global text_editor_elf_start
global text_editor_elf_end
text_editor_elf_start:
    incbin "build/text_editor.elf"
text_editor_elf_end:

global file_manager_elf_start
global file_manager_elf_end
file_manager_elf_start:
    incbin "build/file_manager.elf"
file_manager_elf_end:

global settings_elf_start
global settings_elf_end
settings_elf_start:
    incbin "build/settings.elf"
settings_elf_end:

global task_manager_elf_start
global task_manager_elf_end
task_manager_elf_start:
    incbin "build/task_manager.elf"
task_manager_elf_end:

global wm_stubborn_elf_start
global wm_stubborn_elf_end
wm_stubborn_elf_start:
    incbin "build/wm_stubborn.elf"
wm_stubborn_elf_end:

global wm_zorder_elf_start
global wm_zorder_elf_end
wm_zorder_elf_start:
    incbin "build/wm_zorder.elf"
wm_zorder_elf_end:

global shutdown_elf_start
global shutdown_elf_end
shutdown_elf_start:
    incbin "build/shutdown.elf"
shutdown_elf_end:

global reboot_elf_start
global reboot_elf_end
reboot_elf_start:
    incbin "build/reboot.elf"
reboot_elf_end:
