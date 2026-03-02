/*
 * Sample B-CAS Card Reader (sample_bcas_readcard.c)
 *
 * Simple example of reading B-CAS card information using libpcsc
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../include/scard.h"

/* B-CAS specific APDU commands */
#define BCAS_CLA    0xB0
#define BCAS_INS_SELECT 0xA4

/* B-CAS card AID (Application ID) */
static const BYTE bcas_aid[] = {0xA0, 0x00, 0x00, 0x00, 0xB0, 0x01, 0x00};

void print_buffer(const char *label, const BYTE *buf, size_t len)
{
	size_t i;

	printf("%s: ", label);
	for (i = 0; i < len; i++)
		printf("%02X ", buf[i]);
	printf("\n");
}

void print_error(const char *func, LONG ret)
{
	printf("Error in %s: 0x%08lX (%s)\n",
	       func, (unsigned long)ret, SCardGetErrorMessage(ret));
}

int main(int argc, char *argv[])
{
	SCARDCONTEXT ctx = NULL;
	SCARDHANDLE card = NULL;
	LONG ret;
	char readers[256] = {0};
	DWORD readers_len = sizeof(readers);
	DWORD active_protocol;
	BYTE apdu_cmd[300];
	BYTE apdu_resp[300];
	DWORD apdu_resp_len;

	printf("=== B-CAS Smart Card Reader Sample ===\n\n");

	/* Step 1: Establish context */
	printf("Step 1: Establishing context...\n");
	ret = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx);
	if (ret != SCARD_S_SUCCESS) {
		print_error("SCardEstablishContext", ret);
		return 1;
	}
	printf("Context established successfully.\n\n");

	/* Step 2: List available readers */
	printf("Step 2: Listing available readers...\n");
	ret = SCardListReaders(ctx, NULL, readers, &readers_len);
	if (ret != SCARD_S_SUCCESS) {
		print_error("SCardListReaders", ret);
		SCardReleaseContext(ctx);
		return 1;
	}

	char *reader = readers;
	int reader_count = 0;
	while (reader[0] != '\0') {
		printf("  Reader %d: %s\n", reader_count, reader);
		reader += strlen(reader) + 1;
		reader_count++;
	}

	if (reader_count == 0) {
		printf("No readers available.\n");
		SCardReleaseContext(ctx);
		return 1;
	}

	printf("\n");

	/* Step 3: Detect card */
	printf("Step 3: Detecting card...\n");
	struct scard_readerstate state = {
		.reader = readers,
		.current_state = SCARD_UNKNOWN,
		.event_state = SCARD_UNKNOWN
	};

	ret = SCardGetStatusChange(ctx, 1000, &state, 1);
	if (ret != SCARD_S_SUCCESS) {
		print_error("SCardGetStatusChange", ret);
		SCardReleaseContext(ctx);
		return 1;
	}

	if (!(state.event_state & SCARD_PRESENT)) {
		printf("No card detected in reader.\n");
		SCardReleaseContext(ctx);
		return 1;
	}

	printf("Card detected in reader.\n\n");

	/* Step 4: Connect to card */
	printf("Step 4: Connecting to card...\n");
	ret = SCardConnect(ctx, readers,
	                    SCARD_SHARE_SHARED,
	                    SCARD_PROTOCOL_T0,
	                    &card, &active_protocol);
	if (ret != SCARD_S_SUCCESS) {
		print_error("SCardConnect", ret);
		SCardReleaseContext(ctx);
		return 1;
	}

	printf("Connected to card successfully.\n");
	printf("Active protocol: %s\n",
	       (active_protocol == SCARD_PROTOCOL_T0) ? "T=0" :
	       (active_protocol == SCARD_PROTOCOL_T1) ? "T=1" : "Unknown");
	printf("\n");

	/* Step 5: Select B-CAS application */
	printf("Step 5: Selecting B-CAS application...\n");
	apdu_cmd[0] = BCAS_CLA;
	apdu_cmd[1] = BCAS_INS_SELECT;
	apdu_cmd[2] = 0x04;  /* P1: Select by AID */
	apdu_cmd[3] = 0x00;  /* P2: First occurrence */
	apdu_cmd[4] = sizeof(bcas_aid);  /* Lc */
	memcpy(&apdu_cmd[5], bcas_aid, sizeof(bcas_aid));

	apdu_resp_len = sizeof(apdu_resp);
	ret = SCardTransmit(card, NULL,
	                     apdu_cmd, 5 + sizeof(bcas_aid),
	                     NULL,
	                     apdu_resp, &apdu_resp_len);
	if (ret != SCARD_S_SUCCESS) {
		print_error("SCardTransmit (SELECT)", ret);
		goto cleanup;
	}

	print_buffer("Response", apdu_resp, apdu_resp_len);
	if (apdu_resp_len >= 2) {
		printf("Status: %02X %02X\n",
		       apdu_resp[apdu_resp_len - 2],
		       apdu_resp[apdu_resp_len - 1]);
	}

	printf("\n");

	/* Step 6: Read card status */
	printf("Step 6: Reading card information...\n");
	printf("B-CAS card communication established.\n");
	printf("Further card operations can be performed using SCardTransmit().\n");

	printf("\n=== Sample completed successfully ===\n");

cleanup:
	/* Step 7: Disconnect and release resources */
	if (card) {
		ret = SCardDisconnect(card, SCARD_RESET_CARD);
		if (ret != SCARD_S_SUCCESS)
			print_error("SCardDisconnect", ret);
	}

	if (ctx) {
		ret = SCardReleaseContext(ctx);
		if (ret != SCARD_S_SUCCESS)
			print_error("SCardReleaseContext", ret);
	}

	return 0;
}
