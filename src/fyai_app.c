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

void fyai_scope_state_init(struct fyai_scope *scope, struct fyai_loop *loop,
			   struct fyai_diag *diag)
{
	memset(scope, 0, sizeof(*scope));
	scope->loop = loop;
	scope->diag = diag;
}

void fyai_tty_init(struct fyai_tty *tty, struct fyai_loop *loop,
		   struct fyai_diag *diag)
{
	memset(tty, 0, sizeof(*tty));
	tty->loop = loop;
	tty->diag = diag;
}

void fyai_skills_init(struct fyai_skills *skills, struct fyai_arena *arena,
		      struct fyai_transient *transient)
{
	memset(skills, 0, sizeof(*skills));
	skills->arena = arena;
	skills->transient = transient;
}

void fyai_toolspec_init(struct fyai_toolspec *ts, struct fyai_skills *skills)
{
	memset(ts, 0, sizeof(*ts));
	ts->skills = skills;
	/* Not the all-zero word: that is an empty sequence. */
	ts->spec = fy_invalid;
	ts->allow = fy_invalid;
}

void fyai_branch_state_init(struct fyai_branch_state *bs,
			    struct fyai_arena *arena, struct fyai_diag *diag)
{
	memset(bs, 0, sizeof(*bs));
	bs->arena = arena;
	bs->diag = diag;
	/* A zeroed fy_generic is an empty sequence, not fy_invalid. */
	bs->config = fy_invalid;
	bs->catalog = fy_invalid;
	bs->skills = fy_invalid;
	bs->branches = fy_invalid;
	bs->prev = fy_invalid;
	bs->desc = fy_invalid;
	bs->agent = fy_invalid;
	bs->store = fy_invalid;
	bs->project_state = fy_invalid;
	bs->tool_changes = fy_seq_empty;
}

void fyai_usage_init(struct fyai_usage *usage, struct fyai_diag *diag)
{
	memset(usage, 0, sizeof(*usage));
	usage->diag = diag;
	/* A zeroed fy_generic is an empty sequence, not fy_invalid. */
	usage->agents_root = fy_invalid;
	usage->head = fy_invalid;
}

void fyai_turnstate_init(struct fyai_turnstate *turn, struct fyai_diag *diag)
{
	memset(turn, 0, sizeof(*turn));
	turn->diag = diag;
	/* A zeroed fy_generic is an empty sequence, not fy_invalid. */
	turn->last_message = fy_invalid;
	turn->output_schema = fy_invalid;
	turn->structured_output = fy_invalid;
	turn->last_token_extents = fy_invalid;
	turn->media_pending = fy_seq_empty;
}

int fyai_app_init(struct fyai_app *app)
{
	int rc;

	memset(app, 0, sizeof(*app));
	rc = fyai_diag_setup(&app->diag);
	if (rc)
		return -1;
	app->cfg.diag = &app->diag;
	fyai_loop_init(&app->loop, &app->diag);
	fyai_cred_init(&app->cred, &app->loop);
	fyai_transport_state_init(&app->transport, &app->loop, &app->cred);
	fyai_mcp_state_init(&app->mcp, &app->loop, &app->cred, &app->transient);
	fyai_waitq_init(&app->waitq, &app->loop, &app->diag);
	fyai_askq_init(&app->askq, &app->diag);
	fyai_scope_state_init(&app->scope, &app->loop, &app->diag);
	fyai_tty_init(&app->tty, &app->loop, &app->diag);
	fyai_skills_init(&app->skills, &app->arena, &app->transient);
	fyai_toolspec_init(&app->toolspec, &app->skills);
	fyai_branch_state_init(&app->bstate, &app->arena, &app->diag);
	fyai_usage_init(&app->usage, &app->diag);
	fyai_turnstate_init(&app->turn, &app->diag);
	fyai_link_init(&app->link, &app->loop, &app->cred);
	fyai_arena_init(&app->arena);
	fyai_transient_init(&app->transient);
	app->transient.parent_gb = &app->arena.gb;
	return 0;
}

void fyai_app_cleanup(struct fyai_app *app)
{
	if (!app)
		return;
	/* The loop and the configuration report into the diagnostics, so they
	 * go first. */
	/* The transient builder stacks on the arena, so it goes first. */
	fyai_transient_cleanup(&app->transient);
	fyai_arena_release(&app->arena);
	fyai_loop_cleanup(&app->loop);
	fyai_config_cleanup(&app->cfg);
	app->cfg.diag = NULL;
	fyai_diag_cleanup(&app->diag);
}
