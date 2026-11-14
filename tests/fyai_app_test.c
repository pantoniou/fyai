/*
 * fyai_app_test.c - unit tests for the components that the program shares
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_CONFIG

#include <stdio.h>
#include <string.h>

#include "fyai.h"
#include "fyai_app.h"
#include "fyai_diag.h"

#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(app, wires_diag, app_wires_diag)
FYAI_TEST_ENTRY(app, cleanup_twice, app_cleanup_twice)
FYAI_TEST_ENTRY(app, cfg_without_sink_is_muted, app_cfg_without_sink_is_muted)

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
