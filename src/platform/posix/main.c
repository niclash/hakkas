#include <stdio.h>
#include <stdlib.h>

#include "engine/version.h"
#include "platform/pal.h"
#include "platform/posix/log_stderr.h"
#include "system/log.h"

int main(void)
{
	log_add_sink(posix_log_stderr, NULL);
	if (pal_init() != PAL_OK)
		return EXIT_FAILURE;

	printf("hakkas %s\n", hakkas_version());
	return 0;
}
