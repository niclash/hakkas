/*
 * Reading a password (02 §2.2, 05 §1.2). See platform/posix/password.h.
 */
#include "platform/posix/password.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "system/crypto.h"

const char *posix_password_status_str(enum posix_password_status st)
{
	switch (st) {
	case POSIX_PASSWORD_OK:
		return "ok";
	case POSIX_PASSWORD_EMPTY:
		return "empty password";
	case POSIX_PASSWORD_NO_INPUT:
		return "no password given";
	case POSIX_PASSWORD_TOO_LONG:
		return "password too long";
	case POSIX_PASSWORD_MISMATCH:
		return "the passwords do not match";
	case POSIX_PASSWORD_IO:
		return "cannot read the password";
	}
	return "?";
}

/* Drop the line end: "\n", and a "\r" before it. */
static size_t strip(char *buf, size_t n)
{
	if (n > 0 && buf[n - 1] == '\n')
		buf[--n] = '\0';
	if (n > 0 && buf[n - 1] == '\r')
		buf[--n] = '\0';
	return n;
}

/* One line from a stream that is not a terminal. */
static enum posix_password_status read_stream(FILE *in, char *buf, size_t cap, size_t *len)
{
	size_t n = 0;
	int ch = EOF;
	while (n + 1 < cap && (ch = fgetc(in)) != EOF) {
		buf[n++] = (char)ch;
		if (ch == '\n')
			break;
	}
	buf[n] = '\0';
	if (ferror(in))
		return POSIX_PASSWORD_IO;
	if (n == 0)
		return POSIX_PASSWORD_NO_INPUT;
	if (buf[n - 1] != '\n' && n + 1 >= cap && (ch = fgetc(in)) != EOF && ch != '\n')
		return POSIX_PASSWORD_TOO_LONG;
	n = strip(buf, n);
	*len = n;
	return n == 0 ? POSIX_PASSWORD_EMPTY : POSIX_PASSWORD_OK;
}

/* ---- The terminal ---------------------------------------------------------------- */

static const int stop_signals[] = {SIGINT, SIGTERM, SIGQUIT, SIGTSTP};
#define STOP_SIGNAL_COUNT 4

static volatile sig_atomic_t caught;

static void on_signal(int sig)
{
	caught = sig;
}

/* One line from the terminal fd with echo off. */
static enum posix_password_status read_tty_line(int fd, FILE *prompt_out, const char *prompt,
						char *buf, size_t cap, size_t *len)
{
	struct termios saved, quiet;
	if (tcgetattr(fd, &saved) != 0)
		return POSIX_PASSWORD_IO;
	quiet = saved;
	quiet.c_lflag &= (tcflag_t) ~(ECHO | ECHOE | ECHOK | ECHONL);

	/* Catch the stop signals (no SA_RESTART, so read fails with EINTR),
	 * so the terminal is restored before they take effect. */
	struct sigaction act, old[STOP_SIGNAL_COUNT];
	memset(&act, 0, sizeof(act));
	act.sa_handler = on_signal;
	sigemptyset(&act.sa_mask);
	caught = 0;
	for (int i = 0; i < STOP_SIGNAL_COUNT; i++)
		sigaction(stop_signals[i], &act, &old[i]);

	fputs(prompt, prompt_out);
	fflush(prompt_out);
	enum posix_password_status st = POSIX_PASSWORD_OK;
	if (tcsetattr(fd, TCSAFLUSH, &quiet) != 0)
		st = POSIX_PASSWORD_IO;
	size_t n = 0;
	bool line_end = false, too_long = false;
	while (st == POSIX_PASSWORD_OK && !line_end && caught == 0) {
		char ch;
		ssize_t r = read(fd, &ch, 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			st = POSIX_PASSWORD_IO;
		} else if (r == 0) {
			break;
		} else if (ch == '\n') {
			line_end = true;
		} else if (n + 1 < cap) {
			buf[n++] = ch;
		} else {
			too_long = true; /* read the rest of the line anyway */
		}
		ch = 0;
	}
	buf[n] = '\0';
	tcsetattr(fd, TCSAFLUSH, &saved);
	fputc('\n', prompt_out);
	fflush(prompt_out);

	for (int i = 0; i < STOP_SIGNAL_COUNT; i++)
		sigaction(stop_signals[i], &old[i], NULL);
	if (caught != 0) {
		crypto_zeroize(buf, cap);
		raise(caught); /* now with its usual effect */
		return POSIX_PASSWORD_IO;
	}
	if (st != POSIX_PASSWORD_OK)
		return st;
	if (too_long)
		return POSIX_PASSWORD_TOO_LONG;
	if (n == 0 && !line_end)
		return POSIX_PASSWORD_NO_INPUT;
	n = strip(buf, n);
	*len = n;
	return n == 0 ? POSIX_PASSWORD_EMPTY : POSIX_PASSWORD_OK;
}

enum posix_password_status posix_read_password(FILE *in, FILE *prompt_out, bool confirm,
						char *buf, size_t cap, size_t *len)
{
	*len = 0;
	if (cap < 2)
		return POSIX_PASSWORD_IO;
	int fd = fileno(in);
	if (fd < 0 || !isatty(fd))
		return read_stream(in, buf, cap, len);

	enum posix_password_status st =
		read_tty_line(fd, prompt_out, "Password: ", buf, cap, len);
	if (st != POSIX_PASSWORD_OK || !confirm)
		return st;
	char again[1025];
	size_t again_len = 0;
	st = read_tty_line(fd, prompt_out, "Retype password: ", again,
			   sizeof(again) < cap ? sizeof(again) : cap, &again_len);
	if (st == POSIX_PASSWORD_OK && (again_len != *len || memcmp(again, buf, *len) != 0))
		st = POSIX_PASSWORD_MISMATCH;
	crypto_zeroize(again, sizeof(again));
	if (st != POSIX_PASSWORD_OK) {
		crypto_zeroize(buf, cap);
		*len = 0;
	}
	return st;
}
