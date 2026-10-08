/* SPDX-License-Identifier: MIT */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(event_wait, stale_agent_dropped, event_wait_stale_agent_dropped)
FYAI_TEST_ENTRY(event_wait, stale_session_dropped, event_wait_stale_session_dropped)
FYAI_TEST_ENTRY(event_wait, unowned_delivered, event_wait_unowned_delivered)
FYAI_TEST_ENTRY(event_wait, drop_owner, event_wait_drop_owner)
FYAI_TEST_ENTRY(event_wait, peek_seen, event_wait_peek_seen)
FYAI_TEST_ENTRY(event_wait, background_reset, event_wait_background_reset)

static struct fyai_ctx wait_ctx;

static void wait_ctx_setup(void)
{
	memset(&wait_ctx, 0, sizeof(wait_ctx));
}

static void wait_ctx_teardown(void)
{
	fyai_events_release(&wait_ctx);
}

/*
 * A wait report for a sub-agent with no live job is stale: the branch
 * answers no question, so the report is dropped instead of submitted.
 */
int event_wait_stale_agent_dropped(void)
{
	char *event;
	int rc;

	wait_ctx_setup();
	rc = fyai_event_inject_owned(&wait_ctx,
			strdup("[agent 'greeter' is waiting for input]"),
			FYAI_EVENT_OWNER_AGENT, strdup("main/agent:greeter"));
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(fyai_event_queued(&wait_ctx));
	event = fyai_event_take_live(&wait_ctx);
	FYAI_TCHECK(!event);
	FYAI_TCHECK(!fyai_event_queued(&wait_ctx));
	wait_ctx_teardown();
	printf("ok - a stale sub-agent wait is dropped\n");
	return 0;
}

/* The same holds for a shell session with no live owner. */
int event_wait_stale_session_dropped(void)
{
	char *event;
	int rc;

	wait_ctx_setup();
	rc = fyai_event_inject_owned(&wait_ctx,
			strdup("[shell 'asker' is waiting for input]"),
			FYAI_EVENT_OWNER_SESSION, strdup("asker"));
	FYAI_TCHECK(!rc);
	event = fyai_event_take_live(&wait_ctx);
	FYAI_TCHECK(!event);
	FYAI_TCHECK(!fyai_event_queued(&wait_ctx));
	wait_ctx_teardown();
	printf("ok - a stale session wait is dropped\n");
	return 0;
}

/* Unowned events (waits, agent control input) are always delivered. */
int event_wait_unowned_delivered(void)
{
	char *event;
	int rc;

	wait_ctx_setup();
	rc = fyai_event_inject(&wait_ctx, strdup("wait 'later' fired"));
	FYAI_TCHECK(!rc);
	event = fyai_event_take_live(&wait_ctx);
	FYAI_TCHECK(event);
	FYAI_TCHECK(!strcmp(event, "wait 'later' fired"));
	free(event);
	FYAI_TCHECK(!fyai_event_queued(&wait_ctx));
	wait_ctx_teardown();
	printf("ok - an unowned event is delivered\n");
	return 0;
}

/*
 * Settling an owner purges its queued waits and spares the rest: the
 * model never turns on a program that has ended.
 */
int event_wait_drop_owner(void)
{
	char *event;
	int rc;

	wait_ctx_setup();
	rc = fyai_event_inject_owned(&wait_ctx,
			strdup("[agent 'greeter' is waiting for input]"),
			FYAI_EVENT_OWNER_AGENT, strdup("main/agent:greeter"));
	FYAI_TCHECK(!rc);
	rc = fyai_event_inject(&wait_ctx, strdup("wait 'later' fired"));
	FYAI_TCHECK(!rc);
	fyai_events_drop_agent(&wait_ctx, "main/agent:greeter");
	FYAI_TCHECK(fyai_event_queued(&wait_ctx));
	event = fyai_event_take_live(&wait_ctx);
	FYAI_TCHECK(event);
	FYAI_TCHECK(!strcmp(event, "wait 'later' fired"));
	free(event);
	FYAI_TCHECK(!fyai_event_queued(&wait_ctx));
	wait_ctx_teardown();
	printf("ok - settling an owner purges its waits\n");
	return 0;
}

/*
 * A waiter reads an event and leaves it queued: a second waiter reads the
 * same one, the turn loop does not deliver it, and the purge removes it.
 */
int event_wait_peek_seen(void)
{
	char *first, *second;

	wait_ctx_setup();
	FYAI_TCHECK(!fyai_event_inject(&wait_ctx, strdup("[monitor 'm'] line")));
	first = fyai_event_peek_prefix(&wait_ctx, "[monitor 'm'");
	second = fyai_event_peek_prefix(&wait_ctx, "[monitor 'm'");
	FYAI_TCHECK(first && second && !strcmp(first, second));
	free(first);
	free(second);
	FYAI_TCHECK(!fyai_event_peek_prefix(&wait_ctx, "[agent 'm' "));
	FYAI_TCHECK(!fyai_event_queued(&wait_ctx));
	FYAI_TCHECK(!fyai_event_take_live(&wait_ctx));
	fyai_events_purge_seen(&wait_ctx);
	FYAI_TCHECK(!wait_ctx.events);
	wait_ctx_teardown();
	printf("ok - waiters read an event without taking it\n");
	return 0;
}

/* A new conversation gets no event of the old one. */
int event_wait_background_reset(void)
{
	wait_ctx_setup();
	FYAI_TCHECK(!fyai_event_inject(&wait_ctx, strdup("[wait 'w' fired]")));
	FYAI_TCHECK(fyai_event_queued(&wait_ctx));
	fyai_background_reset(&wait_ctx);
	FYAI_TCHECK(!fyai_event_queued(&wait_ctx));
	FYAI_TCHECK(!fyai_event_take_live(&wait_ctx));
	wait_ctx_teardown();
	printf("ok - a conversation change drops queued events\n");
	return 0;
}
