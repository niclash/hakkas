#include "platform/posix/log_stderr.h"

#include <stdio.h>

void posix_log_stderr(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)level;
	fprintf(stderr, "%.*s\n", (int)len, line);
}
