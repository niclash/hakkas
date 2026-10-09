#include <stdio.h>

#include "engine/version.h"

int main(void)
{
	printf("hakkas %s\n", hakkas_version());
	return 0;
}
