/*
 * fyai_app_test.c - unit tests for the components that the program shares
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_CONFIG

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_app.h"
#include "fyai_diag.h"
#include "fyai_event.h"
#include "fyai_arena.h"
#include "fyai_loop.h"
#include "fyai_transient.h"

#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(app, wires_diag, app_wires_diag)
FYAI_TEST_ENTRY(app, cleanup_twice, app_cleanup_twice)
FYAI_TEST_ENTRY(app, cfg_without_sink_is_muted, app_cfg_without_sink_is_muted)
FYAI_TEST_ENTRY(app, wires_loop, app_wires_loop)
FYAI_TEST_ENTRY(app, loop_tells_an_interrupt, app_loop_tells_an_interrupt)
FYAI_TEST_ENTRY(app, cleanup_releases_loop, app_cleanup_releases_loop)
FYAI_TEST_ENTRY(app, release_keeps_dump, app_release_keeps_dump)
FYAI_TEST_ENTRY(app, arena_release_order, app_arena_release_order)
FYAI_TEST_ENTRY(app, transient_follows_arena, app_transient_follows_arena)
FYAI_TEST_ENTRY(app, transient_stacks_on_slot, app_transient_stacks_on_slot)
FYAI_TEST_ENTRY(app, transient_tells_its_release, app_transient_tells_its_release)
FYAI_TEST_ENTRY(app, wires_transport, app_wires_transport)
FYAI_TEST_ENTRY(app, wires_mcp, app_wires_mcp)
FYAI_TEST_ENTRY(app, wires_link, app_wires_link)
FYAI_TEST_ENTRY(app, wires_cred, app_wires_cred)
FYAI_TEST_ENTRY(app, cred_holds_login_state, app_cred_holds_login_state)
FYAI_TEST_ENTRY(app, cred_keeps_secret_until_release, app_cred_keeps_secret_until_release)
FYAI_TEST_ENTRY(app, other_loop_keeps_handlers, app_other_loop_keeps_handlers)

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
	fyai_diag_reset(&app.diag);
	FYAI_TCHECK(!fyai_diag_got_error(&app.diag));
	/* An error after the first is demoted, so a warning shows the sink. */
	fyai_warning(&ctx, "raised by the context");
	FYAI_TCHECK(fyai_diag_pending(&app.diag));
	fyai_diag_reset(&app.diag);

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
	struct fyai_ctx ctx = { 0 };
	struct fyai_diag *diag;

	diag = fyai_cfg_diag(&cfg);
	FYAI_TCHECK(diag);
	fyai_cfg_error(&cfg, "a scratch copy raised this");
	FYAI_TCHECK(!fyai_diag_got_error(diag));
	FYAI_TCHECK(!fyai_diag_pending(diag));
	FYAI_TCHECK(!fyai_cfg_diag(NULL));

	/* The muted sink is shared, so nobody may turn it on. */
	fyai_diag_enable(diag, FYAIET_NOTICE, true);
	fyai_diag_set_source(diag, true);
	fyai_cfg_notice(&cfg, "still nothing");
	FYAI_TCHECK(!fyai_diag_pending(diag));
	FYAI_TCHECK(!fyai_diag_source(diag));

	/* A context reaches the same sink as its configuration. */
	ctx.cfg = &cfg;
	FYAI_TCHECK(fyai_ctx_diag(&ctx) == diag);
	printf("ok - a configuration with no sink reports nothing\n");
	return 0;
}

static enum fyai_event_action timer_cb(const struct fyai_event *ev)
{
	*(bool *)ev->userdata = true;
	return FYAIEA_STOP;
}

/* The loop component reports into the diagnostics of the app and runs timers. */
int app_wires_transport(void)
{
	struct fyai_app app;

	FYAI_TCHECK(!fyai_app_init(&app));
	FYAI_TCHECK(app.transport.loop == &app.loop && app.transport.cred == &app.cred);
	FYAI_TCHECK(app.transport.role == FYAI_TRANSPORT_NONE && !app.transport.client);
	FYAI_TCHECK(app.transport.ctl == -1 && app.transport.envfd == -1);
	fyai_app_cleanup(&app);
	printf("ok - the transport component starts with no client\n");
	return 0;
}

int app_wires_mcp(void)
{
	struct fyai_app app;

	FYAI_TCHECK(!fyai_app_init(&app));
	FYAI_TCHECK(app.mcp.loop == &app.loop && app.mcp.cred == &app.cred);
	FYAI_TCHECK(app.mcp.transient == &app.transient);
	FYAI_TCHECK(!app.mcp.conns && !app.mcp.stopping && !app.mcp.invocation);
	FYAI_TCHECK(fy_is_invalid(app.mcp.tools) && fy_is_invalid(app.mcp.active_tools));
	fyai_app_cleanup(&app);
	printf("ok - the MCP component starts with no connection\n");
	return 0;
}

int app_wires_link(void)
{
	struct fyai_app app;

	FYAI_TCHECK(!fyai_app_init(&app));
	FYAI_TCHECK(app.link.loop == &app.loop && app.link.cred == &app.cred);
	FYAI_TCHECK(!app.link.curl && !app.link.headers && !app.link.curl_state);
	FYAI_TCHECK(!app.link.status && !app.link.error[0]);
	fyai_app_cleanup(&app);
	printf("ok - the connection component stands on the loop and the login\n");
	return 0;
}

int app_wires_cred(void)
{
	struct fyai_app app;

	FYAI_TCHECK(!fyai_app_init(&app));
	FYAI_TCHECK(app.cred.loop == &app.loop);
	FYAI_TCHECK(!app.cred.ctx && !app.cred.refresh && !app.cred.timer);
	fyai_app_cleanup(&app);
	printf("ok - the credential component stands on the loop of the app\n");
	return 0;
}

int app_cred_holds_login_state(void)
{
	struct fyai_app app;

	FYAI_TCHECK(!fyai_app_init(&app));
	FYAI_TCHECK(fy_is_invalid(app.cred.auth.registrations));
	FYAI_TCHECK(!app.cred.auth.access_token && !app.cred.auth_gb);
	FYAI_TCHECK(!app.cred.auth_detached);
	fyai_app_cleanup(&app);
	printf("ok - the login state starts empty in the credential component\n");
	return 0;
}

int app_cred_keeps_secret_until_release(void)
{
	struct fyai_app app;
	struct fyai_ctx ctx;
	char *secret = NULL;

	FYAI_TCHECK(!fyai_app_init(&app));
	memset(&ctx, 0, sizeof(ctx));
	ctx.cfg = &app.cfg;
	ctx.cred = &app.cred;
	fyai_cred_bind(ctx.cred, &ctx);
	FYAI_TCHECK(!fyai_cred_mem_set(&ctx, "key", "value"));
	FYAI_TCHECK(!fyai_cred_get(&ctx, "mem:key", &secret, NULL));
	FYAI_TCHECK(secret && !strcmp(secret, "value"));
	free(secret);
	fyai_cred_release(&ctx);
	FYAI_TCHECK(ctx.cred == &app.cred && !app.cred.nmem);
	FYAI_TCHECK(fyai_cred_get(&ctx, "mem:key", &secret, NULL) == -ENOENT);
	fyai_app_cleanup(&app);
	printf("ok - release clears the secrets and keeps the component\n");
	return 0;
}

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

/*
 * The release that the run does destroys what stands on the loop, and drops
 * the hook that names the run. The dump output stays for a late report.
 */
int app_release_keeps_dump(void)
{
	struct fyai_app app;
	int fd;

	FYAI_TCHECK(!fyai_app_init(&app));
	fd = dup(STDERR_FILENO);
	FYAI_TCHECK(fd >= 0);
	app.loop.dump_fd = fd;
	app.loop.on_interrupt = interrupt_hook;
	app.loop.on_interrupt_data = &interrupt_told;
	FYAI_TCHECK(fyai_loop_get(&app.loop));

	fyai_loop_release(&app.loop);
	FYAI_TCHECK(!app.loop.el);
	FYAI_TCHECK(!app.loop.on_interrupt && !app.loop.on_interrupt_data);
	FYAI_TCHECK(app.loop.dump_fd == fd);
	FYAI_TCHECK(fcntl(fd, F_GETFD) >= 0);

	fyai_app_cleanup(&app);
	FYAI_TCHECK(app.loop.dump_fd == -1);
	FYAI_TCHECK(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
	printf("ok - the release keeps the dump output until the app ends\n");
	return 0;
}

/* The handlers belong to the loop that installed them. */
int app_other_loop_keeps_handlers(void)
{
	struct fyai_app app;
	struct fyai_loop other;

	FYAI_TCHECK(!fyai_app_init(&app));
	fyai_loop_init(&other, &app.diag);
	FYAI_TCHECK(fyai_loop_get(&app.loop));
	FYAI_TCHECK(!fyai_event_interrupt_open(&app.loop, 5000));

	/* Another loop closes its handlers: the ones of the app stay. */
	fyai_event_interrupt_close(&other);
	kill(getpid(), SIGINT);
	FYAI_TCHECK(app.loop.interrupt_pending);
	FYAI_TCHECK(!other.interrupt_pending);

	fyai_event_interrupt_ack(&app.loop);
	fyai_app_cleanup(&app);
	printf("ok - a loop does not remove the handlers of another\n");
	return 0;
}

/* The transient builder stacks on the builder that its slot holds now. */
int app_transient_stacks_on_slot(void)
{
	struct fy_generic_builder_cfg gbcfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *first, *second;
	struct fyai_transient t;
	fy_generic stored;

	fyai_transient_init(&t);
	first = fy_generic_builder_create(&gbcfg);
	FYAI_TCHECK(first);
	t.parent_gb = &first;
	stored = fy_value(first, "kept in the parent");
	FYAI_TCHECK(fy_is_valid(stored));

	FYAI_TCHECK(!fyai_transient_setup(&t));
	FYAI_TCHECK(t.gb && t.allocator && !t.autorelease);
	/* A second setup keeps the builder, as an owner adopts it. */
	FYAI_TCHECK(!fyai_transient_setup(&t));
	FYAI_TCHECK(fyai_transient_get(&t) == t.gb);
	fyai_transient_cleanup(&t);
	FYAI_TCHECK(!t.gb && !t.allocator);

	/* The arena replaces its builder: the slot gives the new one. */
	second = fy_generic_builder_create(&gbcfg);
	FYAI_TCHECK(second);
	fy_generic_builder_destroy(first);
	first = second;
	FYAI_TCHECK(!fyai_transient_setup(&t));
	FYAI_TCHECK(t.gb);
	fyai_transient_cleanup(&t);
	fy_generic_builder_destroy(first);
	printf("ok - the transient builder reads its parent from the slot\n");
	return 0;
}

static int released;

static void note_release(void *data)
{
	(*(int *)data)++;
}

/* The owner is told when a builder goes, and not when there is none. */
int app_transient_tells_its_release(void)
{
	struct fyai_transient t;

	fyai_transient_init(&t);
	t.on_release = note_release;
	t.on_release_data = &released;

	fyai_transient_cleanup(&t);
	FYAI_TCHECK(released == 0);

	/* A builder that an idle operation made is released by the idle loop. */
	FYAI_TCHECK(fyai_transient_get(&t));
	FYAI_TCHECK(t.autorelease);
	fyai_transient_cleanup(&t);
	FYAI_TCHECK(released == 1);
	FYAI_TCHECK(!t.autorelease);
	printf("ok - the owner is told when the transient builder goes\n");
	return 0;
}

/* The release destroys the overlay before the builder that it stacks on. */
int app_arena_release_order(void)
{
	struct fy_auto_allocator_cfg acfg = { .scenario = FYAST_PER_TAG_FREE_DEDUP };
	struct fy_generic_builder_cfg gbcfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fyai_arena arena;

	fyai_arena_init(&arena);
	fyai_arena_release(&arena);

	arena.durable_allocator = fy_allocator_create("auto", &acfg);
	FYAI_TCHECK(arena.durable_allocator);
	gbcfg.allocator = arena.durable_allocator;
	arena.durable_gb = fy_generic_builder_create(&gbcfg);
	FYAI_TCHECK(arena.durable_gb);
	arena.overlay_allocator = fy_allocator_create("auto", &acfg);
	FYAI_TCHECK(arena.overlay_allocator);
	gbcfg.allocator = arena.overlay_allocator;
	gbcfg.parent = arena.durable_gb;
	arena.gb = fy_generic_builder_create(&gbcfg);
	FYAI_TCHECK(arena.gb && arena.gb != arena.durable_gb);
	arena.refs_head = 42;

	fyai_arena_release(&arena);
	FYAI_TCHECK(!arena.gb && !arena.durable_gb);
	FYAI_TCHECK(!arena.durable_allocator && !arena.overlay_allocator);
	FYAI_TCHECK(!arena.refs_head);
	/* A builder that is the durable one is destroyed once. */
	arena.durable_allocator = fy_allocator_create("auto", &acfg);
	gbcfg.allocator = arena.durable_allocator;
	gbcfg.parent = NULL;
	arena.durable_gb = fy_generic_builder_create(&gbcfg);
	arena.gb = arena.durable_gb;
	FYAI_TCHECK(arena.gb);
	fyai_arena_release(&arena);
	FYAI_TCHECK(!arena.gb && !arena.durable_gb && !arena.durable_allocator);
	fyai_arena_release(&arena);
	printf("ok - the arena releases the overlay, the builder and the allocator\n");
	return 0;
}

/* The transient builder of the app follows the working builder of the arena. */
int app_transient_follows_arena(void)
{
	struct fy_generic_builder_cfg gbcfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fyai_app app;
	fy_generic kept;

	FYAI_TCHECK(!fyai_app_init(&app));
	FYAI_TCHECK(app.transient.parent_gb == &app.arena.gb);
	app.arena.gb = app.arena.durable_gb = fy_generic_builder_create(&gbcfg);
	FYAI_TCHECK(app.arena.gb);
	kept = fy_value(app.arena.gb, "in the arena");
	FYAI_TCHECK(fy_is_valid(kept));
	FYAI_TCHECK(!fyai_transient_setup(&app.transient));
	FYAI_TCHECK(app.transient.gb);
	fyai_app_cleanup(&app);
	FYAI_TCHECK(!app.arena.gb && !app.transient.gb);
	printf("ok - the transient builder follows the arena of the app\n");
	return 0;
}
