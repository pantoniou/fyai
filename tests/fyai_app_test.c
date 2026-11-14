/*
 * fyai_app_test.c - unit tests for the components that the program shares
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_CONFIG

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_app.h"
#include "fyai_diag.h"
#include "fyai_event.h"
#include "fyai_loop.h"

#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(app, wires_diag, app_wires_diag)
FYAI_TEST_ENTRY(app, cleanup_twice, app_cleanup_twice)
FYAI_TEST_ENTRY(app, cfg_without_sink_is_muted, app_cfg_without_sink_is_muted)
FYAI_TEST_ENTRY(app, wires_loop, app_wires_loop)
FYAI_TEST_ENTRY(app, loop_tells_an_interrupt, app_loop_tells_an_interrupt)
FYAI_TEST_ENTRY(app, cleanup_releases_loop, app_cleanup_releases_loop)

/* The configuration reports into the sink that the app owns. */
int app_wires_diag(void)
{
	struct fyai_app app;
	struct fyai_ctx ctx = { 0 };

	FYAI_TCHECK(!fyai_app_init(&app));
	ctx.cfg = &app.cfg;
	FYAI_TCHECK(app.cfg.diag == &app.diag);
	FYAI_TCHECK(fyai_cfg_diag(&app.cfg) == &app.diag);
	FYAI_TCHECK(fyai_ctx_diag(&ctx) == &app.diag);

	fyai_cfg_error(&app.cfg, "raised by the configuration");
	FYAI_TCHECK(fyai_diag_got_error(&app.diag));
	fyai_error(&ctx, "raised by the context");
	fyai_diag_reset(&app.diag);
	FYAI_TCHECK(!fyai_diag_got_error(&app.diag));

	fyai_app_cleanup(&app);
	FYAI_TCHECK(!app.cfg.diag);
	printf("ok - the configuration and the context report into the app\n");
	return 0;
}

/* The release of the components is safe to repeat. */
int app_cleanup_twice(void)
{
	struct fyai_app app;

	FYAI_TCHECK(!fyai_app_init(&app));
	fyai_app_cleanup(&app);
	fyai_app_cleanup(&app);
	fyai_app_cleanup(NULL);
	printf("ok - the app is released twice without harm\n");
	return 0;
}

/*
 * A configuration with no sink is a scratch copy: it reports nothing. A
 * missing configuration is not, and reports at once.
 */
int app_cfg_without_sink_is_muted(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_diag *diag;

	diag = fyai_cfg_diag(&cfg);
	FYAI_TCHECK(diag);
	fyai_cfg_error(&cfg, "a scratch copy raised this");
	FYAI_TCHECK(!fyai_diag_got_error(diag));
	FYAI_TCHECK(!fyai_diag_pending(diag));
	FYAI_TCHECK(!fyai_cfg_diag(NULL));
	printf("ok - a configuration with no sink reports nothing\n");
	return 0;
}

static enum fyai_event_action timer_cb(const struct fyai_event *ev)
{
	*(bool *)ev->userdata = true;
	return FYAIEA_STOP;
}

/* The loop component reports into the diagnostics of the app and runs timers. */
int app_wires_loop(void)
{
	struct fyai_app app;
	struct fyai_event_loop *el;
	struct fyai_event_source *src = NULL;
	volatile bool fired = false;

	FYAI_TCHECK(!fyai_app_init(&app));
	FYAI_TCHECK(app.loop.diag == &app.diag);
	el = fyai_loop_get(&app.loop);
	FYAI_TCHECK(el);
	FYAI_TCHECK(fyai_loop_get(&app.loop) == el);
	FYAI_TCHECK(!fyai_event_add_timer(el, 1, 0, timer_cb, (void *)&fired,
					  &src));
	FYAI_TCHECK(fyai_event_loop_run_until(el, &fired, 5000) >= 0);
	FYAI_TCHECK(fired);
	fyai_app_cleanup(&app);
	printf("ok - the loop of the app runs a timer\n");
	return 0;
}

static int interrupt_told;

static void interrupt_hook(void *data)
{
	(*(int *)data)++;
}

/* A pending interrupt tells the owner of the loop through its hook. */
int app_loop_tells_an_interrupt(void)
{
	struct fyai_app app;
	struct fyai_event_loop *el;

	FYAI_TCHECK(!fyai_app_init(&app));
	app.loop.on_interrupt = interrupt_hook;
	app.loop.on_interrupt_data = &interrupt_told;
	el = fyai_loop_get(&app.loop);
	FYAI_TCHECK(el);
	FYAI_TCHECK(!fyai_event_interrupt_open(&app.loop, 5000));

	kill(getpid(), SIGINT);
	FYAI_TCHECK(app.loop.interrupt_pending);
	FYAI_TCHECK(fyai_event_loop_step(el, 2000) >= 0);
	FYAI_TCHECK(interrupt_told == 1);

	fyai_event_interrupt_ack(&app.loop);
	fyai_app_cleanup(&app);
	printf("ok - the loop tells its owner about an interrupt\n");
	return 0;
}

/* The release of the app destroys the loop and the pools. */
int app_cleanup_releases_loop(void)
{
	struct fyai_app app;

	FYAI_TCHECK(!fyai_app_init(&app));
	FYAI_TCHECK(fyai_loop_get(&app.loop));
	fyai_app_cleanup(&app);
	FYAI_TCHECK(!app.loop.el);
	FYAI_TCHECK(!app.loop.event_loop_pool);
	FYAI_TCHECK(!app.loop.event_source_pool);
	printf("ok - the release of the app destroys the loop\n");
	return 0;
}
