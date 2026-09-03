/* user_space/libc/include/ar.h - M98
 *
 * The ar(1) archive format, as one magic string and one 60-byte member
 * header. This is a file format older than most operating systems and
 * identical on all of them - the fields are ASCII decimal and octal so
 * that a person with `od` can read them, which is also why there is
 * nothing to get wrong porting it. GNU make's arscan.c (the code behind
 * `lib(member)` prerequisites) named it first here; binutils carries
 * its own copy of the same struct for the same format.
 */
#pragma once

#define ARMAG  "!<arch>\n" /* the leading magic */
#define SARMAG 8           /* its length */

#define ARFMAG "`\n"       /* per-member trailing magic */

struct ar_hdr {
    char ar_name[16]; /* member name, '/'-terminated (GNU) or blank-padded */
    char ar_date[12]; /* decimal seconds since the epoch */
    char ar_uid[6];   /* decimal uid */
    char ar_gid[6];   /* decimal gid */
    char ar_mode[8];  /* octal mode */
    char ar_size[10]; /* decimal member size in bytes */
    char ar_fmag[2];  /* ARFMAG */
};
