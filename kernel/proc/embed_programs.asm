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

global cp_elf_start
global cp_elf_end
cp_elf_start:
    incbin "build/cp.elf"
cp_elf_end:

global audiograb_elf_start
global audiograb_elf_end
audiograb_elf_start:
    incbin "build/audiograb.elf"
audiograb_elf_end:

global libctest_elf_start
global libctest_elf_end
libctest_elf_start:
    incbin "build/libctest.elf"
libctest_elf_end:

global netconf_elf_start
global netconf_elf_end
netconf_elf_start:
    incbin "build/netconf.elf"
netconf_elf_end:

global nettime_elf_start
global nettime_elf_end
nettime_elf_start:
    incbin "build/nettime.elf"
nettime_elf_end:

global tcptest_elf_start
global tcptest_elf_end
tcptest_elf_start:
    incbin "build/tcptest.elf"
tcptest_elf_end:

global racetest_elf_start
global racetest_elf_end
racetest_elf_start:
    incbin "build/racetest.elf"
racetest_elf_end:

global console_elf_start
global console_elf_end
console_elf_start:
    incbin "build/console.elf"
console_elf_end:

global nslookup_elf_start
global nslookup_elf_end
nslookup_elf_start:
    incbin "build/nslookup.elf"
nslookup_elf_end:

global fetch_elf_start
global fetch_elf_end
fetch_elf_start:
    incbin "build/fetch.elf"
fetch_elf_end:

global httpd_elf_start
global httpd_elf_end
httpd_elf_start:
    incbin "build/httpd.elf"
httpd_elf_end:

global caps_elf_start
global caps_elf_end
caps_elf_start:
    incbin "build/caps.elf"
caps_elf_end:

global captest_elf_start
global captest_elf_end
captest_elf_start:
    incbin "build/captest.elf"
captest_elf_end:

global nettest_elf_start
global nettest_elf_end
nettest_elf_start:
    incbin "build/nettest.elf"
nettest_elf_end:

global whetstone_elf_start
global whetstone_elf_end
whetstone_elf_start:
    incbin "build/whetstone.elf"
whetstone_elf_end:

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

global sh_elf_start
global sh_elf_end
sh_elf_start:
    incbin "build/sh.elf"
sh_elf_end:

global memtest_elf_start
global memtest_elf_end
memtest_elf_start:
    incbin "build/memtest.elf"
memtest_elf_end:

global fonttest_elf_start
global fonttest_elf_end
fonttest_elf_start:
    incbin "build/fonttest.elf"
fonttest_elf_end:

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

global wm_faulter_elf_start
global wm_faulter_elf_end
wm_faulter_elf_start:
    incbin "build/wm_faulter.elf"
wm_faulter_elf_end:

global wm_crash_elf_start
global wm_crash_elf_end
wm_crash_elf_start:
    incbin "build/wm_crash.elf"
wm_crash_elf_end:

global badptr_elf_start
global badptr_elf_end
badptr_elf_start:
    incbin "build/badptr.elf"
badptr_elf_end:

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

; M75: `env` prints what a process inherited, and `envtest` is the
; self-test fixture that proves it landed somewhere real. Appended at the
; end of this list rather than beside the other coreutils on purpose -
; kernel.c seeds files in FOR_EACH_EMBEDDED_PROGRAM order and the M22
; self-test grades "launcher slot 0 is hello, the first file ever
; seeded", so an insertion anywhere above shifts an inode this project
; asserts on.
global env_elf_start
global env_elf_end
env_elf_start:
    incbin "build/env.elf"
env_elf_end:

global envtest_elf_start
global envtest_elf_end
envtest_elf_start:
    incbin "build/envtest.elf"
envtest_elf_end:

global sigtest_elf_start
global sigtest_elf_end
sigtest_elf_start:
    incbin "build/sigtest.elf"
sigtest_elf_end:

global treewalk_elf_start
global treewalk_elf_end
treewalk_elf_start:
    incbin "build/treewalk.elf"
treewalk_elf_end:

global mmaptest_elf_start
global mmaptest_elf_end
mmaptest_elf_start:
    incbin "build/mmaptest.elf"
mmaptest_elf_end:

global threadtest_elf_start
global threadtest_elf_end
threadtest_elf_start:
    incbin "build/threadtest.elf"
threadtest_elf_end:

global lazytest_elf_start
global lazytest_elf_end
lazytest_elf_start:
    incbin "build/lazytest.elf"
lazytest_elf_end:

global vmtest_elf_start
global vmtest_elf_end
vmtest_elf_start:
    incbin "build/vmtest.elf"
vmtest_elf_end:

global forktest_elf_start
global forktest_elf_end
forktest_elf_start:
    incbin "build/forktest.elf"
forktest_elf_end:

global exectest_elf_start
global exectest_elf_end
exectest_elf_start:
    incbin "build/exectest.elf"
exectest_elf_end:

global jobtest_elf_start
global jobtest_elf_end
jobtest_elf_start:
    incbin "build/jobtest.elf"
jobtest_elf_end:
