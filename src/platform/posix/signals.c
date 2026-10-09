#include "platform/posix/signals.h"

#include <signal.h>
#include <stdbool.h>
#include <string.h>

static struct sigaction saved_int, saved_term;
static bool installed;

static void on_stop_signal(int sig)
{
	(void)sig;
	pal_request_stop(); /* async-signal-safe */
}

enum pal_err posix_stop_signals_install(void)
{
	if (installed)
		return PAL_ERR_INVALID;
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_stop_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGINT, &sa, &saved_int) != 0)
		return PAL_ERR_UNAVAILABLE;
	if (sigaction(SIGTERM, &sa, &saved_term) != 0) {
		sigaction(SIGINT, &saved_int, NULL);
		return PAL_ERR_UNAVAILABLE;
	}
	installed = true;
	return PAL_OK;
}

void posix_stop_signals_restore(void)
{
	if (!installed)
		return;
	sigaction(SIGINT, &saved_int, NULL);
	sigaction(SIGTERM, &saved_term, NULL);
	installed = false;
}
