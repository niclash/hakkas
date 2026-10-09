/*
 * Minimal unit-test harness. Each test file is one executable registered with
 * CTest; it exits non-zero if any check failed.
 *
 *	static void test_something(void) { CHECK(1 + 1 == 2); }
 *	int main(void) { RUN(test_something); return test_result(); }
 */
#ifndef HAKKAS_TEST_H
#define HAKKAS_TEST_H

#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);   \
			test_failures++;                                                           \
		}                                                                                  \
	} while (0)

#define CHECK_STREQ(actual, expected)                                                              \
	do {                                                                                       \
		const char *a_ = (actual), *e_ = (expected);                                       \
		if (a_ == NULL || strcmp(a_, e_) != 0) {                                           \
			fprintf(stderr, "%s:%d: CHECK_STREQ(%s): got \"%s\", expected \"%s\"\n",   \
				__FILE__, __LINE__, #actual, a_ ? a_ : "(null)", e_);              \
			test_failures++;                                                           \
		}                                                                                  \
	} while (0)

#define RUN(fn)                                                                                    \
	do {                                                                                       \
		int before_ = test_failures;                                                       \
		fn();                                                                              \
		fprintf(stderr, "%s %s\n", test_failures == before_ ? "PASS" : "FAIL", #fn);      \
	} while (0)

static inline int test_result(void)
{
	return test_failures == 0 ? 0 : 1;
}

#endif
