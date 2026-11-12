/*
 * fyai_shell_session_test.c - tests for the stop of a terminal session
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_test.h"
#include "fyai_tools.h"
#include "fyai_tools_int.h"
#include "fyai_uimodel.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(shell_session, stop_survives_end, shell_session_stop)

static void remote_input(void *data, const char *name, const char *bytes,
			 size_t len)
{
	(void)data; (void)name; (void)bytes; (void)len;
}

static void remote_resize(void *data, const char *name, int rows, int cols)
{
	(void)data; (void)name; (void)rows; (void)cols;
}

static void remote_close(void *data, const char *name, bool force)
{
	(void)data; (void)name; (void)force;
}

static void remote_dismiss(void *data, const char *name)
{
	(void)data; (void)name;
}

static const struct fyai_tools_remote remote = {
	.input = remote_input,
	.resize = remote_resize,
	.close = remote_close,
	.dismiss = remote_dismiss,
};

/* The first item of the work store, which holds the one mirrored session. */
static fy_generic work_item(struct fyai_ctx *ctx)
{
	return fy_get_at(fy_get(fyai_uimodel_get(ctx, FYAI_UI_WORK), "items",
				fy_invalid), 0, fy_invalid);
}

/*
 * A mirror takes the stop of its worker from an item. The reason of the stop
 * is what a client reads after the end, so it must survive the end, and an
 * idle stop is a close as well.
 */
int shell_session_stop(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	struct fyai_shell_session *sess;
	fy_generic item, shown;
	int i;
	static const struct {
		bool closing, timed_out;
		bool want_closing, want_timed_out;
	} cases[] = {
		{ false, false, false, false },
		{ true, false, true, false },
		{ true, true, true, true },
		{ false, true, true, true },
	};

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
		item = fy_mapping("name", "t", "command", "true",
				  "kind", "shell");
		sess = fyai_tools_mirror_open(&ctx, item, &remote, NULL);
		FYAI_TCHECK(sess != NULL);
		if (!sess)
			break;
		/* The positive control: a session that runs has no stop. */
		fyai_tools_work_update(&ctx);
		shown = work_item(&ctx);
		FYAI_TCHECK(fy_is_invalid(fy_get(shown, "closing",
						 fy_invalid)));
		item = fy_mapping("name", "t", "command", "true",
				  "kind", "shell", "exited", true,
				  "closing", cases[i].closing,
				  "timed_out", cases[i].timed_out);
		(void)fyai_tools_mirror_update(sess, item);
		fyai_tools_work_update(&ctx);
		shown = work_item(&ctx);
		FYAI_TCHECK(fy_get(shown, "exited", false));
		FYAI_TCHECK(fy_get(shown, "closing", false) ==
			    cases[i].want_closing);
		FYAI_TCHECK(fy_get(shown, "timed_out", false) ==
			    cases[i].want_timed_out);
		fyai_tools_mirror_drop(sess);
	}
	fyai_uimodel_release(&ctx);
	return 0;
}
