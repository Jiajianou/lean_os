#include "panic.h"

#include "drivers/klog.h"

void panic(const char *msg) {
    klog_puts("\n*** KERNEL PANIC: ");
    klog_puts(msg);
    klog_puts(" ***\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
