/*
 * fyai_app.h - the components that the whole program shares
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_APP_H
#define FYAI_APP_H

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_agent_state.h"
#include "fyai_arena.h"
#include "fyai_jobs.h"
#include "fyai_askq.h"
#include "fyai_branch_state.h"
#include "fyai_cred.h"
#include "fyai_loop.h"
#include "fyai_transient.h"
#include "fyai_turnstate.h"
#include "fyai_usage.h"
#include "fyai_waitq.h"
#include "fyai_transport_state.h"
#include "fyai_link.h"

/*
 * The root of the program: it owns each component that more than one part of
 * the program uses, and it outlives the context. main() declares one and
 * every later object borrows from it (doc/context-components.md).
 *
 * A component is a struct that owns its state and names its dependencies as
 * pointers to other components, set before it is used. fyai_app_init() makes
 * the components in dependency order and wires the pointers;
 * fyai_app_cleanup() destroys them in the reverse order.
 */
struct fyai_app {
	struct fyai_diag diag;		/* depends on nothing */
	struct fyai_loop loop;		/* depends on diag */
	struct fyai_arena arena;	/* depends on nothing yet */
	struct fyai_transient transient; /* depends on the arena builder */
	struct fyai_transport_state transport; /* depends on loop and cred */
	struct fyai_mcp mcp;		/* depends on loop, cred, transient */
	struct fyai_waitq waitq;	/* depends on loop and diag */
	struct fyai_askq askq;		/* depends on diag */
	struct fyai_scope scope;	/* depends on loop and diag */
	struct fyai_tty tty;		/* depends on loop and diag */
	struct fyai_skills skills;	/* depends on arena and transient */
	struct fyai_toolspec toolspec;	/* depends on skills */
	struct fyai_branch_state bstate; /* depends on arena and diag */
	struct fyai_usage usage;	/* depends on diag */
	struct fyai_turnstate turn;	/* depends on diag */
	struct fyai_jobs jobs;		/* depends on loop and diag */
	struct fyai_agent_state astate;	/* depends on bstate and diag */
	struct fyai_link link;		/* depends on loop and cred */
	struct fyai_cred cred;		/* depends on loop */
	struct fyai_cfg cfg;		/* depends on diag */
};

/*
 * Make the components of @app and wire their dependencies. @app is zeroed
 * first. Returns 0, or -1 when a component cannot be made; nothing is left
 * to release then.
 */
int fyai_app_init(struct fyai_app *app);

/* Release what the components hold. The diagnostics are drained first. */
void fyai_app_cleanup(struct fyai_app *app);

#endif
