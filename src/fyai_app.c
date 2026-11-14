/*
 * fyai_app.c - the components that the whole program shares
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_INIT

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "fyai_app.h"
#include "fyai_config.h"

int fyai_app_init(struct fyai_app *app)
{
	int rc;

	memset(app, 0, sizeof(*app));
	rc = fyai_diag_setup(&app->diag);
	if (rc)
		return -1;
	app->cfg.diag = &app->diag;
	fyai_loop_init(&app->loop, &app->diag);
	fyai_transient_init(&app->transient);
	return 0;
}

void fyai_app_cleanup(struct fyai_app *app)
{
	if (!app)
		return;
	/* The loop and the configuration report into the diagnostics, so they
	 * go first. */
	fyai_transient_cleanup(&app->transient);
	fyai_loop_cleanup(&app->loop);
	fyai_config_cleanup(&app->cfg);
	app->cfg.diag = NULL;
	fyai_diag_cleanup(&app->diag);
}
