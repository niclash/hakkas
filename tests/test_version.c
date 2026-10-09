#include "test.h"

#include "engine/version.h"
#include "hakkas_version.h"

static void test_version_is_the_build_version(void)
{
	CHECK_STREQ(hakkas_version(), HAKKAS_VERSION_STRING);
	CHECK(strlen(hakkas_version()) > 0);
}

int main(void)
{
	RUN(test_version_is_the_build_version);
	return test_result();
}
