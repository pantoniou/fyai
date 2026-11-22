/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FLEET_WATCH_H
#define FYAI_FLEET_WATCH_H

#include <stdbool.h>
#include <stddef.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct fyai_loop;
struct fyai_waitq;
struct fyai_fleet_watcher;

/*
 * Root-session supervision of fleet members (doc/fyai-fleet-sdd.md, sec. 10).
 * Keeps one server connection per member started for an open task and delivers
 * member state transitions to the model as events:
 *
 *   [session 'NAME' idle]      body contains the last assistant reply
 *   [session 'NAME' waiting]   a prompt awaits an answer
 *   [session 'NAME' ended]     the member worker exited
 *
 * Open tasks act as persistent subscriptions: a restarted root reattaches to
 * them so no member is orphaned. Embedded in struct fyai_app (ctx->fleet).
 */
struct fyai_fleet_watch {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_waitq *waitq;

	struct fyai_fleet_watcher *watchers;
	bool restored;			/* open tasks already restored */
};

void fyai_fleet_watch_init(struct fyai_fleet_watch *fw, struct fyai_loop *loop,
			   struct fyai_waitq *waitq);

/*
 * Subscribe to state updates for live session @name (PROJECT:BRANCH) until it
 * exits. No-op if already watched. Returns 0, or -1 on error.
 */
int fyai_fleet_watch_add(struct fyai_ctx *ctx, const char *name);

/*
 * Restore open tasks for a root session once: watch running members and
 * restart members with pending work from a dead process
 * (`[session 'NAME' restarted: ...]`). No-op for non-root sessions. Errors are
 * traced without aborting the session.
 */
void fyai_fleet_watch_restore(struct fyai_ctx *ctx);

/* Count of currently supervised members. */
size_t fyai_fleet_watch_live(struct fyai_ctx *ctx);

/* Free closed watcher connections; must be called outside frame dispatch. */
void fyai_fleet_watch_reap(struct fyai_ctx *ctx);

/* Close all watcher connections during shutdown. */
void fyai_fleet_watch_cleanup(struct fyai_fleet_watch *fw);

/*
 * The UI Markdown of the fleet panel of the page from the rows of the member
 * `fleet` of the panels store, {name, state, last, title}, or NULL when there is no
 * row. A row has a mark of its state; the mark of a running member blinks
 * with @phase. @blinks, when not NULL, tells whether a member runs, that is
 * whether the next phase changes the text. The caller frees it.
 */
char *fyai_fleet_panel_source(fy_generic rows, int phase, bool *blinks);

/* Drop inherited watcher connections in a forked child without closing fds. */
void fyai_fleet_watch_abandon(struct fyai_fleet_watch *fw);

#endif
