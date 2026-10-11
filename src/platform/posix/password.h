/*
 * Reading a password for `hakkas user add|passwd` (02 §2.2, 05 §1.2): never
 * from argv.
 *
 * Decided (plan task 16, recorded in 05 §1.2):
 *  - in is a terminal: the prompt goes to prompt_out (stderr), echo is
 *    turned off while the line is read straight from the descriptor (no
 *    stdio buffer keeps a copy), then restored; with confirm the password is
 *    asked twice and both must match. SIGINT, SIGTERM, SIGQUIT and SIGTSTP
 *    while echo is off restore the terminal first, then take their usual
 *    effect.
 *  - in is not a terminal (a pipe or file, for scripts and tests): one line
 *    is read, without a prompt and without confirmation.
 *  - The line ends at a newline (a CR before it is dropped too) or at the end
 *    of input. An empty password, no input at all, or a line longer than
 *    cap - 1 bytes is refused.
 *
 * The caller zeroizes buf after use (crypto_zeroize).
 */
#ifndef HAKKAS_PLATFORM_POSIX_PASSWORD_H
#define HAKKAS_PLATFORM_POSIX_PASSWORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum posix_password_status {
	POSIX_PASSWORD_OK = 0,
	POSIX_PASSWORD_EMPTY = -1,    /* an empty line */
	POSIX_PASSWORD_NO_INPUT = -2, /* the end of input before any byte */
	POSIX_PASSWORD_TOO_LONG = -3, /* longer than cap - 1 bytes */
	POSIX_PASSWORD_MISMATCH = -4, /* the two entries differ */
	POSIX_PASSWORD_IO = -5,	      /* reading or the terminal failed */
};

/* A short, static description of st, for log lines. */
const char *posix_password_status_str(enum posix_password_status st);

/* Reads a password from in into buf (cap bytes), NUL-terminated, its length
 * in *len; see above. */
enum posix_password_status posix_read_password(FILE *in, FILE *prompt_out, bool confirm,
						char *buf, size_t cap, size_t *len);

#endif
