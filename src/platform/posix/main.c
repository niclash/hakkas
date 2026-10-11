/*
 * The POSIX runtime's entry point: the console log sink, the PAL, then the
 * command line (platform/posix/cli.h, 05 §1.2).
 */
#include <stdio.h>
#include <stdlib.h>

#include "platform/pal.h"
#include "platform/posix/cli.h"
#include "platform/posix/log_stderr.h"
#include "system/log.h"

int main(int argc, char *argv[])
{
	log_add_sink(posix_log_stderr, NULL);
	enum pal_err err = pal_init();
	if (err != PAL_OK) {
		log_error("cannot initialize the platform: %s", pal_err_str(err));
		return CLI_EXIT_FAILURE;
	}
	int rc = cli_main(argc, argv, stdin, stdout);
	if (fflush(stdout) != 0 && rc == CLI_EXIT_OK)
		rc = CLI_EXIT_FAILURE;
	return rc;
}
