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

/*
 * Toggle the fleet panel for this session by updating `fleet/panel` and
 * republishing the panel. Returns 0, or -1 on error.
 */
int fyai_fleet_watch_panel_set(struct fyai_ctx *ctx, bool on);

/* Count of currently supervised members. */
size_t fyai_fleet_watch_live(struct fyai_ctx *ctx);

/* Free closed watcher connections; must be called outside frame dispatch. */
void fyai_fleet_watch_reap(struct fyai_ctx *ctx);

/* Close all watcher connections during shutdown. */
void fyai_fleet_watch_cleanup(struct fyai_fleet_watch *fw);

/* The marks of the states of a member, which the theme makes. */
enum fyai_fleet_mark {
	FYAI_FLEET_MARK_RUNNING,
	FYAI_FLEET_MARK_WAITING,
	FYAI_FLEET_MARK_IDLE,
	FYAI_FLEET_MARK_ENDED,
	FYAI_FLEET_MARKS
};

/*
 * Render fleet panel Markdown from panel store rows ({name, state, last,
 * title}). Returns NULL if @rows is not a sequence (non-root session); an
 * empty sequence produces a placeholder line. Each row starts in the gutter
 * with the UI Markdown of its state in @marks, by enum fyai_fleet_mark; the
 * placeholder with the empty gutter @blank. If @blinks is non-NULL, sets
 * *@blinks to true when any member is running. Caller frees the returned
 * string.
 */
char *fyai_fleet_panel_source(fy_generic rows, const char *const *marks,
			      const char *blank, bool *blinks);

/* Drop inherited watcher connections in a forked child without closing fds. */
void fyai_fleet_watch_abandon(struct fyai_fleet_watch *fw);

#endif
