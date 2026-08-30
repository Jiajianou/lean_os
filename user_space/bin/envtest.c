/* user_space/bin/envtest.c - M75's fixture
 *
 * The program the [m75] boot self-test spawns twice, with two different
 * environments and from two different directories. What it proves is
 * stated as a round trip rather than as an echo: it reads a name out of
 * its environment, asks the kernel where it is, and then writes a file
 * using ONLY a relative name - so the file lands wherever the caller put
 * this process, and the self-test grades which one that was.
 *
 * "An unchanged string coming back proves nothing" is the milestone's own
 * wording, and it is the reason none of this program's output is echoed
 * to stdout: a getenv that returned what was passed in would satisfy a
 * printf test and still leave the environment unreachable from a child.
 * A file at a path nobody spelled out is not something a broken
 * implementation can fake.
 *
 * Exit codes are distinct so a failure names its own cause:
 *   0  everything worked
 *   2  a variable that should have been set was not
 *   3  getcwd failed or came back with something that is not a path
 *   4  a variable that should NOT have been inherited was
 *   5  the relative open/write failed
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "paths.h"
#include "syscall_wrappers.h"

int main(void) {
    const char *name = getenv("M75_OUT");
    const char *body = getenv("M75_BODY");
    if (!name || !name[0] || !body || !body[0]) {
        return 2;
    }
    /* Deliberately absent from the environment both children are given.
     * Without this, an implementation that handed every child the *same*
     * environment - the parent's, ignoring envp entirely - would pass
     * every other check here. */
    if (getenv("M75_ABSENT")) {
        return 4;
    }

    char cwd[PATH_MAX_LEN];
    if (!getcwd(cwd, sizeof(cwd)) || cwd[0] != '/') {
        return 3;
    }

    /* The whole point: a relative name, opened with no directory in it
     * anywhere. If the kernel does not resolve against this process's own
     * directory, this either fails or lands in the wrong place - and the
     * self-test looks in both. */
    long fd = sys_open(name, OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    if (fd < 0) {
        return 5;
    }
    char line[PATH_MAX_LEN + 64];
    int n = 0;
    for (const char *s = cwd; *s && n < (int)sizeof(line) - 2; s++) {
        line[n++] = *s;
    }
    line[n++] = ' ';
    for (const char *s = body; *s && n < (int)sizeof(line) - 1; s++) {
        line[n++] = *s;
    }
    long wrote = sys_write((int)fd, line, (size_t)n);
    sys_close((int)fd);
    return wrote == n ? 0 : 5;
}
