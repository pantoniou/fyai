/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FLEET_WATCH_H
#define FYAI_FLEET_WATCH_H

#include <stdbool.h>

struct fyai_ctx;
struct fyai_loop;
struct fyai_waitq;
struct fyai_fleet_watcher;

/*
 * The supervision of the members of a root session (doc/fyai-fleet-sdd.md,
 * section 10): one connection to the server of each member that it started
 * for an open task. A change of a member reaches the model as an event:
 *
 *   [session 'NAME' idle]      the last answer as the body
 *   [session 'NAME' waiting]   a question waits for an answer
 *   [session 'NAME' ended]     its worker went
 *
 * The open tasks are the subscriptions: a root session that starts takes them
 * up again, so a lost root loses no member. The component is a member of
 * struct fyai_app and is reached as ctx->fleet.
 */
struct fyai_fleet_watch {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_waitq *waitq;

	struct fyai_fleet_watcher *watchers;
	/* The open tasks of the branch were taken up. */
	bool restored;
};

void fyai_fleet_watch_init(struct fyai_fleet_watch *fw, struct fyai_loop *loop,
			   struct fyai_waitq *waitq);

/*
 * Supervise the live session @name (PROJECT:BRANCH) until it ends; a session
 * that is watched already is left as it is. Returns 0, or -1 with the cause
 * reported.
 */
int fyai_fleet_watch_add(struct fyai_ctx *ctx, const char *name);

/*
 * Take up the open tasks of this root session once: supervise each member
 * that runs, and start again each member that a lost process left with work
 * (`[session 'NAME' restarted: ...]`). A session that is not of the root does
 * nothing. Failures are traced; the session goes on.
 */
void fyai_fleet_watch_restore(struct fyai_ctx *ctx);

/* Free the connections that ended; call it outside their frames. */
void fyai_fleet_watch_reap(struct fyai_ctx *ctx);

/* Close every connection: at the end of the run. */
void fyai_fleet_watch_cleanup(struct fyai_fleet_watch *fw);

/* A forked child forgets the connections of its parent. */
void fyai_fleet_watch_abandon(struct fyai_fleet_watch *fw);

#endif
