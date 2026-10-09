#include <stdio.h>

#include "engine/version.h"
#include "platform/posix/log_stderr.h"
#include "system/log.h"

int main(void)
{
	log_add_sink(posix_log_stderr, NULL);

	printf("hakkas %s\n", hakkas_version());
	return 0;
}
