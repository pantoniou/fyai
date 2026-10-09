/*
 * fyai_event_fmt_test.c - tests for the events as the user reads them
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_event_fmt.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(event_fmt, headers, event_fmt_headers)
FYAI_TEST_ENTRY(event_fmt, bodies, event_fmt_bodies)
FYAI_TEST_ENTRY(event_fmt, not_events, event_fmt_not_events)

/* The event as the user reads it equals @want. */
static int event_reads(const char *text, const char *want)
{
	char *s = fyai_event_pretty(text);
	int same = s && !strcmp(s, want);

	if (!same)
		fprintf(stderr, "event %s: got %s\n", text, s ? s : "(null)");
	free(s);
	return same;
}

int event_fmt_headers(void)
{
	FYAI_TCHECK(event_reads("[wait 'w1' fired: timer]",
				"**wait** `w1` fired: timer\n"));
	FYAI_TCHECK(event_reads("[wait 'w1' fired]",
				"**wait** `w1` fired\n"));
	FYAI_TCHECK(event_reads("[agent 'a-1' failed: no key]",
				"**agent** `a-1` failed: no key\n"));
	FYAI_TCHECK(event_reads("[shell 'sh' is waiting for input: Password:]",
				"**shell** `sh` is waiting for input: Password:\n"));
	printf("ok - an event is drawn as its kind, its name and what happened\n");
	return 0;
}

int event_fmt_bodies(void)
{
	/* The result of an agent is Markdown for the user. */
	FYAI_TCHECK(event_reads("[agent 'a' finished]\nDone *it*",
				"**agent** `a` finished\n\nDone *it*\n"));
	/* The lines of a monitor keep their breaks. */
	FYAI_TCHECK(event_reads("[monitor 'm'] one\ntwo",
				"**monitor** `m`\n\n```\none\ntwo\n```\n"));
	FYAI_TCHECK(event_reads("[monitor 'm' ended]",
				"**monitor** `m` ended\n"));
	/* The answers of a question are listed as the tool lists them. */
	FYAI_TCHECK(event_reads("[question 'bg' answered]\n"
		"{\"status\":\"answered\",\"answers\":[{\"id\":\"q1\","
		"\"header\":\"Scope\",\"question\":\"How wide?\","
		"\"selected\":[\"o2\"],\"labels\":[\"medium\"]}]}",
		"**question** `bg` answered\n\n"
		"- **Scope** How wide?  \n  → medium\n"));
	FYAI_TCHECK(event_reads("[question 'bg' declined]\n"
		"the user did not provide an answer",
		"**question** `bg` declined\n\n"
		"the user did not provide an answer\n"));
	printf("ok - the body of an event is drawn under its header\n");
	return 0;
}

int event_fmt_not_events(void)
{
	FYAI_TCHECK(fyai_event_pretty("hello") == NULL);
	FYAI_TCHECK(fyai_event_pretty("") == NULL);
	FYAI_TCHECK(fyai_event_pretty(NULL) == NULL);
	FYAI_TCHECK(fyai_event_pretty("[not an event]") == NULL);
	FYAI_TCHECK(fyai_event_pretty("[Upper 'x' ok]") == NULL);
	FYAI_TCHECK(fyai_event_pretty("[wait 'unclosed") == NULL);
	FYAI_TCHECK(fyai_event_pretty("[wait 'x' no bracket") == NULL);
	printf("ok - a line that is not an event is shown as it is\n");
	return 0;
}
