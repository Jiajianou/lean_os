#include <time.h>

#include "syscall_wrappers.h"

time_t time(time_t *out) {
    long now = sys_time((os_datetime_t *)0);
    if (out) {
        *out = (time_t)now;
    }
    return (time_t)now;
}

clock_t clock(void) {
    /* Milliseconds since boot, which is what CLOCKS_PER_SEC above says
     * this returns. Not CPU time: this OS does not account per-task CPU
     * time at all, and reporting wall time as if it were would be a
     * quieter lie than reporting wall time and saying so. */
    return (clock_t)sys_uptime_ms();
}
