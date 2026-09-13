#include "string_utilities.h"
#include "syscall_wrappers.h"

static void say(const char *s) {
    sys_write(1, s, strlen(s));
}

int main(void) {
    if (sys_audio_claim() == 0) {
        say("[audiograb] FAIL: took the speaker from its owner\n");
        return 1;
    }
    if (sys_beep(440, 50) == 0) {
        say("[audiograb] FAIL: beeped without owning the speaker\n");
        return 2;
    }
    if (sys_audio_volume(0) == 0) {
        say("[audiograb] FAIL: muted a machine it does not own\n");
        return 3;
    }
    say("[audiograb] refused the claim, the beep and the volume - ownership holds.\n");
    return 0;
}
