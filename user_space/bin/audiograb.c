/* user_space/bin/audiograb.c
 *
 * M62's ownership check, from the only place it can honestly be made:
 * another process. The rule this exists to prove is that there is one
 * speaker and one owner - the compositor, or in the boot self-test the
 * kernel task - and that a program which simply asks cannot take it.
 *
 * Exits 0 when it was correctly refused, which is the *success* case and
 * worth saying out loud: this program working means this program failing
 * to do what it tries to do.
 */
#include "str.h"
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
