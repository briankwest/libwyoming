#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static const char *current_test = NULL;

void test_begin(const char *name)
{
	current_test = name;
	tests_run++;
	printf("  TEST: %s ... ", name);
	fflush(stdout);
}

void test_assert(int condition, const char *msg)
{
	if (!condition) {
		printf("FAIL: %s\n", msg);
		tests_failed++;
		/* Don't abort — continue to next test */
	}
}

void test_end(void)
{
	if (tests_failed == 0 || (tests_run > tests_passed + tests_failed)) {
		printf("ok\n");
		tests_passed++;
	}
}

/* Extern test functions */
extern void test_event_roundtrip(void);
extern void test_event_no_data(void);
extern void test_event_with_payload(void);
extern void test_event_multiple(void);
extern void test_client_server_describe(void);
extern void test_client_server_synthesize(void);

int main(void)
{
	printf("libwyoming test suite\n");
	printf("=====================\n\n");

	printf("[event tests]\n");
	test_event_roundtrip();
	test_event_no_data();
	test_event_with_payload();
	test_event_multiple();

	printf("\n[client/server tests]\n");
	test_client_server_describe();
	test_client_server_synthesize();

	printf("\n=====================\n");
	printf("%d tests run, %d passed, %d failed\n",
	       tests_run, tests_passed, tests_failed);

	return tests_failed > 0 ? 1 : 0;
}
