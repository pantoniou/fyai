/*
 * fyai_scope.h - the tool scope component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_SCOPE_H
#define FYAI_SCOPE_H

#include <stdbool.h>

#include "fyai_cover.h"

struct fyai_diag;
struct fyai_event_source;
struct fyai_home;
struct fyai_loop;
struct fyai_orphan;

/*
 * What confines and ends the processes that the tools start: the cover that
 * hides the arena and its holder, the pins that keep the scope of an
 * exchange, the processes a tool left and the timer that ends them, the home
 * of the tools, and the state of the sandbox. The component stands on the
 * loop for the kill timer and on the diagnostics. The owner is struct
 * fyai_app.
 */
struct fyai_scope {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_diag *diag;

	struct fyai_cover cover;	/* how a tool hides the arena, and the holder of it */
	int pins;			/* objects that keep the scope of an exchange */
	bool open;			/* an exchange runs */
	struct fyai_orphan *orphans;	/* processes that a tool left, to end with the scope */
	struct fyai_event_source *orphan_kill;
	struct fyai_home *home;		/* the home of the tools, when a profile gives them one */
	/*
	 * Set inside a forked tool sub-execution once the environment has been
	 * sanitized and the sandbox applied, so inner steps do not derive and
	 * apply the confinement again.
	 */
	bool sandbox_applied;
	bool sandbox_warned;		/* the failure of the sandbox was reported */
};

void fyai_scope_state_init(struct fyai_scope *scope, struct fyai_loop *loop,
			   struct fyai_diag *diag);
/* The child owns neither the holder nor the pins of its parent. */
void fyai_scope_abandon(struct fyai_scope *scope);

#endif
