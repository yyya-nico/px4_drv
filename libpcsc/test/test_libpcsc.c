/*
 * libpcsc Unit Tests (test_libpcsc.c)
 *
 * Basic unit tests for PCSC-Lite API
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/scard.h"

typedef struct {
	const char *name;
	int (*test_func)(void);
	int passed;
	int failed;
} test_case_t;

static int test_establish_release_context(void)
{
	SCARDCONTEXT ctx = NULL;
	LONG ret;

	printf("  Test: Establish and Release Context\n");

	ret = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx);
	if (ret != SCARD_S_SUCCESS) {
		printf("    FAILED: SCardEstablishContext returned 0x%08lX\n",
		       (unsigned long)ret);
		return 0;
	}

	if (!ctx) {
		printf("    FAILED: Context is NULL\n");
		return 0;
	}

	ret = SCardReleaseContext(ctx);
	if (ret != SCARD_S_SUCCESS) {
		printf("    FAILED: SCardReleaseContext returned 0x%08lX\n",
		       (unsigned long)ret);
		return 0;
	}

	printf("    PASSED\n");
	return 1;
}

static int test_invalid_context(void)
{
	char readers[256];
	DWORD readers_len = sizeof(readers);
	LONG ret;

	printf("  Test: Invalid Context Handling\n");

	ret = SCardListReaders(NULL, NULL, readers, &readers_len);
	if (ret == SCARD_E_INVALID_HANDLE) {
		printf("    PASSED\n");
		return 1;
	}

	printf("    FAILED: Expected SCARD_E_INVALID_HANDLE, got 0x%08lX\n",
	       (unsigned long)ret);
	return 0;
}

static int test_list_readers(void)
{
	SCARDCONTEXT ctx = NULL;
	char readers[256] = {0};
	DWORD readers_len = sizeof(readers);
	LONG ret;
	int reader_count = 0;
	char *reader;

	printf("  Test: List Readers\n");

	ret = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx);
	if (ret != SCARD_S_SUCCESS) {
		printf("    FAILED: Cannot establish context\n");
		return 0;
	}

	ret = SCardListReaders(ctx, NULL, readers, &readers_len);
	if (ret != SCARD_S_SUCCESS) {
		printf("    FAILED: SCardListReaders returned 0x%08lX\n",
		       (unsigned long)ret);
		SCardReleaseContext(ctx);
		return 0;
	}

	reader = readers;
	while (reader[0] != '\0') {
		printf("    Found reader: %s\n", reader);
		reader += strlen(reader) + 1;
		reader_count++;
	}

	if (reader_count == 0) {
		printf("    WARNING: No readers available (device not connected?)\n");
	}

	SCardReleaseContext(ctx);
	printf("    PASSED\n");
	return 1;
}

static int test_error_messages(void)
{
	printf("  Test: Error Messages\n");

	const char *msg_success = SCardGetErrorMessage(SCARD_S_SUCCESS);
	if (!msg_success || strlen(msg_success) == 0) {
		printf("    FAILED: Invalid success message\n");
		return 0;
	}

	const char *msg_invalid = SCardGetErrorMessage(SCARD_E_INVALID_HANDLE);
	if (!msg_invalid || strlen(msg_invalid) == 0) {
		printf("    FAILED: Invalid error message\n");
		return 0;
	}

	printf("    Success message: %s\n", msg_success);
	printf("    Error message: %s\n", msg_invalid);
	printf("    PASSED\n");
	return 1;
}

int main(int argc, char *argv[])
{
	test_case_t tests[] = {
		{"Establish/Release Context", test_establish_release_context, 0, 0},
		{"Invalid Context", test_invalid_context, 0, 0},
		{"List Readers", test_list_readers, 0, 0},
		{"Error Messages", test_error_messages, 0, 0},
		{NULL, NULL, 0, 0}
	};

	int i, passed = 0, failed = 0;

	printf("=== libpcsc Unit Tests ===\n\n");

	for (i = 0; tests[i].test_func; i++) {
		printf("Test %d: %s\n", i + 1, tests[i].name);
		if (tests[i].test_func()) {
			tests[i].passed = 1;
			passed++;
		} else {
			tests[i].failed = 1;
			failed++;
		}
		printf("\n");
	}

	printf("=== Test Summary ===\n");
	printf("Total: %d, Passed: %d, Failed: %d\n", i, passed, failed);

	return (failed > 0) ? 1 : 0;
}
