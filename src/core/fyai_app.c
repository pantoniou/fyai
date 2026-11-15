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
#include "fyai_orphan.h"

void fyai_scope_state_init(struct fyai_scope *scope, struct fyai_loop *loop,
			   struct fyai_diag *diag, const struct fyai_cfg *cfg)
{
	memset(scope, 0, sizeof(*scope));
	scope->loop = loop;
	scope->diag = diag;
	scope->cfg = cfg;
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

void fyai_jobs_init(struct fyai_jobs *jobs, struct fyai_loop *loop,
		    struct fyai_diag *diag)
{
	memset(jobs, 0, sizeof(*jobs));
	jobs->loop = loop;
	jobs->diag = diag;
}

void fyai_agent_state_init(struct fyai_agent_state *as,
			   struct fyai_branch_state *bstate,
			   struct fyai_diag *diag)
{
	memset(as, 0, sizeof(*as));
	as->bstate = bstate;
	as->diag = diag;
	/* A zeroed fy_generic is an empty sequence, not fy_invalid. */
	as->spawn = fy_invalid;
}

void fyai_out_init(struct fyai_out *out, struct fyai_loop *loop,
		   struct fyai_diag *diag)
{
	memset(out, 0, sizeof(*out));
	out->loop = loop;
	out->diag = diag;
}

void fyai_uistores_init(struct fyai_uistores *stores, struct fyai_loop *loop,
			struct fyai_diag *diag)
{
	memset(stores, 0, sizeof(*stores));
	stores->loop = loop;
	stores->diag = diag;
}

void fyai_screen_init(struct fyai_screen *screen, struct fyai_loop *loop,
		      struct fyai_diag *diag)
{
	memset(screen, 0, sizeof(*screen));
	screen->loop = loop;
	screen->diag = diag;
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
	fyai_cred_init(&app->cred, &app->loop, &app->cfg, &app->transient,
		       &app->link, &app->transport, &app->diag);
	fyai_transport_state_init(&app->transport, &app->loop, &app->cred, &app->cfg, &app->diag);
	fyai_mcp_state_init(&app->mcp, &app->loop, &app->cred, &app->transient,
			    &app->cfg, &app->arena, &app->link, &app->diag);
	fyai_waitq_init(&app->waitq, &app->loop, &app->diag);
	fyai_askq_init(&app->askq, &app->diag, &app->waitq, &app->cfg);
	fyai_scope_state_init(&app->scope, &app->loop, &app->diag, &app->cfg);
	fyai_tty_init(&app->tty, &app->loop, &app->diag);
	fyai_skills_init(&app->skills, &app->arena, &app->transient);
	fyai_toolspec_init(&app->toolspec, &app->skills);
	fyai_branch_state_init(&app->bstate, &app->arena, &app->diag);
	fyai_usage_init(&app->usage, &app->diag);
	fyai_turnstate_init(&app->turn, &app->diag);
	fyai_jobs_init(&app->jobs, &app->loop, &app->diag);
	fyai_agent_state_init(&app->astate, &app->bstate, &app->diag);
	fyai_out_init(&app->out, &app->loop, &app->diag);
	fyai_uistores_init(&app->uistores, &app->loop, &app->diag);
	fyai_screen_init(&app->screen, &app->loop, &app->diag);
	fyai_link_init(&app->link, &app->loop, &app->cred, &app->diag);
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

/*
 * The abandon() of each component: what a forked child must not keep of the
 * state that describes its parent's process, its screen or its run. A
 * component that makes a resource releases it elsewhere; these only forget.
 */
void fyai_turnstate_abandon(struct fyai_turnstate *turn)
{
	/* A request, a stop, a budget, a gate and a relay belong to one turn. */
	turn->end_requested = false;
	turn->stop = FYAI_TURN_STOP_NONE;
	turn->cost_limit = 0;
	turn->compacting = false;
	turn->gate = NULL;
	turn->gate_data = NULL;
	turn->gate_run = NULL;
	turn->gate_relays = NULL;
	turn->relay_gate = false;
	turn->relay_steps = false;
	turn->output_schema = fy_invalid;
	turn->structured_output = fy_invalid;
	/* The observers are pointers of the parent, a stream to its peer. */
	turn->observe = NULL;
	turn->observe_data = NULL;
	turn->text_observe = NULL;
	turn->text_observe_data = NULL;
	turn->agent_observe = NULL;
	turn->agent_observe_data = NULL;
	/* The extents of its last call point into storage that the child does not use. */
	turn->last_token_extents = fy_invalid;
	turn->response_chain = FYAI_CHAIN_UNLINKED;
	/*
	 * The attachments wait for the next line of the parent, not of this
	 * child; the store stays open to read the bytes of the branch.
	 */
	turn->media_pending = fy_seq_empty;
}

void fyai_usage_abandon(struct fyai_usage *usage)
{
	usage->input = usage->cached = usage->cache_write = 0;
	usage->output = usage->reasoning = usage->total = 0;
	usage->cost = usage->cost_est = 0;
	usage->calls = 0;
	usage->switch_pending = false;
	usage->last_call_input = usage->last_call_output = usage->last_call_total = 0;
}

void fyai_link_abandon(struct fyai_link *link)
{
	link->status = 0;
	link->retry_after = 0;
	link->error[0] = '\0';
}

void fyai_scope_abandon(struct fyai_scope *scope)
{
	fyai_cover_abandon(&scope->cover);
	fyai_orphan_abandon(scope);	/* the processes that the scope keeps */
	scope->pins = 0;
	scope->open = false;
}

void fyai_tty_abandon(struct fyai_tty *tty)
{
	tty->session = NULL;
	tty->winch_src = NULL;
}

void fyai_agent_state_abandon(struct fyai_agent_state *as)
{
	as->registry = NULL;
	as->parent = as->execution;
	as->execution = 0;
}

void fyai_branch_state_abandon(struct fyai_branch_state *bs)
{
	bs->tool_diff_tracking = false;
}

void fyai_mcp_state_abandon(struct fyai_mcp *mcp)
{
	mcp->conns = NULL;
	mcp->tools = fy_invalid;
}

void fyai_transport_state_abandon(struct fyai_transport_state *state)
{
	state->src = NULL;
}

void fyai_jobs_abandon(struct fyai_jobs *jobs)
{
	free(jobs->patch_display);
	jobs->patch_display = NULL;
	jobs->stream = NULL;
}

void fyai_out_abandon(struct fyai_out *out)
{
	(void)out;	/* the sink and the document are released with the objects */
}

void fyai_uistores_abandon(struct fyai_uistores *stores)
{
	(void)stores;	/* the stores and the renderers are released with the objects */
}

void fyai_screen_abandon(struct fyai_screen *screen)
{
	screen->ui = NULL;
	screen->workpane = NULL;
	screen->browser = NULL;
	screen->cmd_call = NULL;
	screen->catalog_update = NULL;
	screen->config_edit = NULL;
}
