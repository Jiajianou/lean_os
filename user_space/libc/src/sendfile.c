#include <errno.h>
#include <stdlib.h>
#include <sys/sendfile.h>
#include <unistd.h>

/* There is no sendfile syscall here, and adding one would buy nothing this
   kernel can deliver: Linux's sendfile is fast because the pages never leave
   the kernel, and this one has no path that splices a file into a socket.
   So it is a read and a write in a loop, which is what the call means, and
   the only thing it adds over writing that loop by hand is the offset
   argument's semantics - which are not obvious and are easy to get wrong. */

#define SENDFILE_CHUNK 65536

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
    char *buffer = malloc(SENDFILE_CHUNK);
    if (!buffer) {
        errno = ENOMEM;
        return -1;
    }
    off_t position = 0;
    if (offset) {
        position = *offset;
    }
    size_t moved = 0;
    int failed = 0;
    while (moved < count) {
        size_t want = count - moved;
        if (want > SENDFILE_CHUNK) {
            want = SENDFILE_CHUNK;
        }
        ssize_t got;
        if (offset) {
            got = pread(in_fd, buffer, want, position);
        } else {
            got = read(in_fd, buffer, want);
        }
        if (got < 0) {
            failed = 1;
            break;
        }
        if (got == 0) {
            break;
        }
        ssize_t written = 0;
        while (written < got) {
            ssize_t n = write(out_fd, buffer + written, (size_t)(got - written));
            if (n <= 0) {
                failed = 1;
                break;
            }
            written += n;
        }
        moved += (size_t)written;
        position += written;
        if (failed || written < got) {
            break;
        }
    }
    free(buffer);
    /* The offset argument is an in-out cursor and NOT the file position:
       when it is given, in_fd's own position must not move, which is why
       the loop above reads with pread. */
    if (offset) {
        *offset = position;
    }
    if (failed && moved == 0) {
        return -1;
    }
    return (ssize_t)moved;
}
