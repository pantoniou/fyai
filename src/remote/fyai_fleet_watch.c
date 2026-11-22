/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_app.h"
#include "fyai_fleet.h"
#include "fyai_fleet_ops.h"
#include "fyai_fleet_watch.h"
#include "fyai_jsonrpc.h"
#include "fyai_remote.h"
#include "fyai_wait.h"
#include "fyai_waitq.h"

#define FYAI_MODULE FYAIEM_REMOTE

/* Persistent supervisor connection to a fleet member's server. */
struct fyai_fleet_watcher {
	struct fyai_fleet_watcher *next;
	struct fyai_fleet_watch *owner;
	struct jsonrpc_conn *conn;
	int in, out;
	char *name;			/* PROJECT:BRANCH */
	bool ended;			/* free it outside its frame */
};

void fyai_fleet_watch_init(struct fyai_fleet_watch *fw, struct fyai_loop *loop,
			   struct fyai_waitq *waitq)
{
	memset(fw, 0, sizeof(*fw));
	fw->loop = loop;
	fw->waitq = waitq;
}

/* Handle a session_state frame and inject a model event when actionable. */
static void fleet_watch_frame(struct jsonrpc_conn *conn, fy_generic frame,
			      void *data)
{
	struct fyai_fleet_watcher *w = data;
	struct fyai_ctx *ctx = fyai_fleet_watch_ctx(w->owner);
	fy_generic state = fy_get(frame, "state", fy_invalid);
	fy_generic last = fy_get(frame, "last", fy_invalid);
	int rc = 0;

	(void)conn;
	if (!fy_equal(fy_get(frame, "type", fy_invalid), "fyai") ||
	    !fy_equal(fy_get(frame, "subtype", fy_invalid), "session_state"))
		return;
	/* Suppress idle transitions without a reply payload. */
	if (fy_equal(state, "idle") && fy_is_string(last))
		rc = fyai_event_injectf(w->owner->waitq, "[session '%s' idle]\n%s",
					w->name, fy_castp(&last, ""));
	else if (fy_equal(state, "waiting"))
		rc = fyai_event_injectf(w->owner->waitq, "[session '%s' waiting]\n"
					"A question of the session waits for an "
					"answer.", w->name);
	else if (fy_equal(state, "ended")) {
		rc = fyai_event_injectf(w->owner->waitq, "[session '%s' ended]",
					w->name);
		w->ended = true;
	} else {
		return;
	}
	if (rc)
		fyai_warning(ctx, "fleet: the change of '%s' was lost", w->name);
	fyai_waiters_kick(ctx);
}

static void fleet_watch_closed(void *data)
{
	struct fyai_fleet_watcher *w = data;

	w->ended = true;
}

static void fleet_watcher_free(struct fyai_fleet_watcher *w)
{
	jsonrpc_conn_destroy(w->conn);
	if (w->in >= 0)
		close(w->in);
	if (w->out >= 0)
		close(w->out);
	free(w->name);
	free(w);
}

int fyai_fleet_watch_add(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_fleet_watch *fw = ctx->fleet;
	struct fyai_fleet_watcher *w;
	const char *branch, *state;
	char *cwd = NULL, *dir = NULL;
	bool image = false;
	int fd = -1, rc = -1;

	for (w = fw->watchers; w; w = w->next)
		if (!w->ended && !strcmp(w->name, name))
			return 0;
	w = calloc(1, sizeof(*w));
	fyai_error_check(ctx, w, out, "fleet: cannot keep the supervision of '%s'", name);
	w->in = w->out = -1;
	w->owner = fw;
	w->name = strdup(name);
	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, w->name && cwd, out, "fleet: cannot keep the name '%s'",
			 name);
	rc = fyai_fleet_name_split(cwd, name, &dir, &branch);
	fyai_error_check(ctx, !rc && dir, out,
			 "fleet: '%s' is not PROJECT:BRANCH of the fleet", name);
	rc = fyai_remote_supervisor_open(ctx, dir, branch, &fd, &image, &state);
	if (rc)
		goto out;
	rc = -1;
	w->in = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	w->out = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	close(fd);
	fyai_error_check(ctx, w->in >= 0 && w->out >= 0, out,
			 "fleet: cannot keep the connection to '%s': %s", name,
			 strerror(errno));
	w->conn = jsonrpc_conn_stdio(ctx, w->out, w->in, 0, "fleet supervisor", NULL);
	fyai_error_check(ctx, w->conn, out, "fleet: cannot open the connection to '%s'",
			 name);
	if (image)
		(void)jsonrpc_conn_set_image(w->conn);
	jsonrpc_conn_on_close(w->conn, fleet_watch_closed, w);
	rc = jsonrpc_conn_frames(w->conn, fleet_watch_frame, w);
	fyai_error_check(ctx, !rc, out, "fleet: cannot read the connection to '%s'",
			 name);
	w->next = fw->watchers;
	fw->watchers = w;
	w = NULL;
	/* Emit waiting event immediately if already blocked on input. */
	if (!strcmp(state, "waiting") &&
	    !fyai_event_injectf(fw->waitq, "[session '%s' waiting]\nA question of "
				"the session waits for an answer.", name))
		fyai_waiters_kick(ctx);
	rc = 0;
out:
	if (w)
		fleet_watcher_free(w);
	free(dir);
	free(cwd);
	return rc ? -1 : 0;
}

/* Trace and clear a non-fatal restore/restart diagnostic. */
static void fleet_watch_trace_failure(struct fyai_ctx *ctx, const char *name)
{
	char *why = fyai_diag_string(fyai_ctx_diag(ctx));

	fyai_diag_reset(fyai_ctx_diag(ctx));
	fyai_diag_tracef("fleet", "'%s' not taken up: %s", name,
			 why && *why ? why : "unknown");
	free(why);
}

void fyai_fleet_watch_restore(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb;
	fy_generic tasks, rows, row, key, task, state;
	const char *name;

	if (ctx->fleet->restored || !fyai_fleet_is_root(ctx))
		return;
	ctx->fleet->restored = true;
	tasks = fyai_fleet_tasks_open(ctx);
	if (fy_empty(tasks))
		return;
	gb = fyai_ctx_transient_gb(ctx);
	if (fyai_fleet_rows_list(ctx, gb, &rows)) {
		fleet_watch_trace_failure(ctx, "the fleet");
		return;
	}
	fy_foreach_key_value(key, task, tasks) {
		(void)key;
		name = fy_gb_intern_string(gb, fy_sprintfa("%s:%s",
			fy_get(task, "project", "."), fy_get(task, "branch", "")));
		state = fy_invalid;
		fy_foreach(row, rows)
			if (!strcmp(fy_sprintfa("%s:%s", fy_get(row, "project", "."),
						fy_get(row, "branch", "")), name))
				state = fy_get(row, "state", fy_invalid);
		/* Revive members whose worker died with work in flight. */
		if (fy_equal(state, "lost")) {
			if (fyai_fleet_revive(ctx, name)) {
				fleet_watch_trace_failure(ctx, name);
				continue;
			}
			(void)fyai_event_injectf(ctx->waitq, "[session '%s' restarted: "
				"its process was lost; it goes on from its last stored "
				"step]", name);
			state = fy_string("running");
		}
		if (fy_any_equal(state, "idle", "running", "waiting") &&
		    fyai_fleet_watch_add(ctx, name))
			fleet_watch_trace_failure(ctx, name);
	}
}

void fyai_fleet_watch_reap(struct fyai_ctx *ctx)
{
	struct fyai_fleet_watcher **link = &ctx->fleet->watchers, *w;

	while ((w = *link)) {
		if (!w->ended) {
			link = &w->next;
			continue;
		}
		*link = w->next;
		fleet_watcher_free(w);
	}
}

void fyai_fleet_watch_cleanup(struct fyai_fleet_watch *fw)
{
	struct fyai_fleet_watcher *w;

	while ((w = fw->watchers)) {
		fw->watchers = w->next;
		fleet_watcher_free(w);
	}
}

void fyai_fleet_watch_abandon(struct fyai_fleet_watch *fw)
{
	struct fyai_fleet_watcher *w;

	/*
	 * Close inherited fds in the forked child without touching the parent's
	 * loop registrations.
	 */
	while ((w = fw->watchers)) {
		fw->watchers = w->next;
		if (w->in >= 0)
			close(w->in);
		if (w->out >= 0)
			close(w->out);
		free(w->name);
		free(w);
	}
	fw->restored = true;
}
