/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_arena.h"
#include "fyai_branch.h"
#include "fyai_buf.h"
#include "fyai_fleet.h"
#include "fyai_fleet_ops.h"
#include "fyai_fleet_watch.h"
#include "fyai_host.h"
#include "fyai_pending.h"
#include "fyai_remote.h"
#include "fyai_storage.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_REMOTE

/* Default directory globs skipped during fleet scan when config omits them. */
static const char *const fleet_exclude_default[] = {
	"node_modules", "build*", ".cache",
};

/* Maximum argv length for delegated `fleet` subprocesses (including NULL). */
#define FLEET_CHILD_ARGS	16

/* True if this process has an open arena (reads must delegate to a child). */
static bool fleet_arena_open(struct fyai_ctx *ctx)
{
	return ctx->arena->durable_allocator != NULL;
}

/*
 * Delegate `fleet SUB --output json REST...` to an arena-free child process
 * and parse its JSON sequence output into *@rowsp. @words is {SUB, REST...,
 * NULL}. Places `--output json` before REST so trailing positional args work.
 */
static int fleet_child(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		       const char *const *words, fy_generic *rowsp)
{
	struct fyai_buf out = { 0 }, err = { 0 };
	int fds[2][2] = { { -1, -1 }, { -1, -1 } }, null = -1, status = 0, i, rc = -1;
	const char *argv[FLEET_CHILD_ARGS];
	struct pollfd pfd[2];
	char self[PATH_MAX], chunk[4096];
	const char *cause;
	size_t argc = 0;
	ssize_t n;
	pid_t pid = -1;

	n = fyai_host_executable(self, sizeof(self));
	fyai_error_check(ctx, n > 0 && (size_t)n < sizeof(self), out,
			 "fleet: cannot find this program");
	self[n] = '\0';
	argv[argc++] = self;
	argv[argc++] = "fleet";
	argv[argc++] = *words++;
	argv[argc++] = "--output";
	argv[argc++] = "json";
	for (; *words && argc < FLEET_CHILD_ARGS - 1; words++)
		argv[argc++] = *words;
	argv[argc] = NULL;
	null = open("/dev/null", O_RDONLY | O_CLOEXEC);
	rc = null < 0 || fyai_host_pipe(fds[0]) || fyai_host_pipe(fds[1]);
	fyai_error_check(ctx, !rc, out, "fleet: cannot make the pipes of the scan: %s",
			 strerror(errno));
	rc = -1;
	pid = fork();
	fyai_error_check(ctx, pid >= 0, out, "fleet: cannot fork the scan: %s",
			 strerror(errno));
	if (!pid) {
		if (dup2(null, STDIN_FILENO) < 0 || dup2(fds[0][1], STDOUT_FILENO) < 0 ||
		    dup2(fds[1][1], STDERR_FILENO) < 0)
			_exit(126);
		fyai_close_fds_from(STDERR_FILENO + 1);
		fyai_loop_restore_signal_mask(ctx->loop);
		signal(SIGPIPE, SIG_DFL);
		execv(self, (char *const *)argv);
		_exit(127);
	}
	for (i = 0; i < 2; i++) {
		close(fds[i][1]);
		fds[i][1] = -1;
		pfd[i].fd = fds[i][0];
		pfd[i].events = POLLIN;
	}
	while (pfd[0].fd >= 0 || pfd[1].fd >= 0) {
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (i = 0; i < 2; i++) {
			if (pfd[i].fd < 0 || !pfd[i].revents)
				continue;
			n = read(pfd[i].fd, chunk, sizeof(chunk));
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0 || fyai_buf_append_data(i ? &err : &out, chunk,
							   (size_t)n))
				pfd[i].fd = -1;
		}
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	pid = -1;
	if (fyai_buf_append_data(&out, "", 1) || fyai_buf_append_data(&err, "", 1))
		goto out;
	/* Strip duplicate module prefix from child stderr before reporting. */
	while (err.len > 1 && (err.data[err.len - 2] == '\n' || err.data[err.len - 2] == ' '))
		err.data[--err.len - 1] = '\0';
	cause = err.data;
	if (!strncmp(cause, "remote: ", strlen("remote: ")))
		cause += strlen("remote: ");
	fyai_error_check(ctx, WIFEXITED(status) && !WEXITSTATUS(status), out,
			 "%s", *cause ? cause : "fleet: the scan failed");
	*rowsp = parse_json_string(gb, out.data);
	fyai_error_check(ctx, fy_is_sequence(*rowsp), out,
			 "fleet: the scan gave no rows");
	rc = 0;
out:
	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
	for (i = 0; i < 2; i++) {
		if (fds[i][0] >= 0)
			close(fds[i][0]);
		if (fds[i][1] >= 0)
			close(fds[i][1]);
	}
	if (null >= 0)
		close(null);
	fyai_buf_free(&out);
	fyai_buf_free(&err);
	return rc;
}

struct fleet_walk {
	struct fy_generic_builder *gb;
	const char *root;
	size_t turns;
	/* Collected {project, sessions, live} entries per project. */
	fy_generic projects;
};

static int fleet_found(const char *dir, void *data)
{
	struct fleet_walk *w = data;
	struct fy_generic_builder *gb = w->gb;
	const char *rel = strcmp(dir, w->root) ? dir + strlen(w->root) + 1 : ".";
	fy_generic rows, live;

	/* Unreadable arenas still appear with an empty stored session list. */
	if (fyai_peek_sessions(fy_sprintfa("%s/.fyai/arena", dir), gb, NULL,
			       w->turns, &rows))
		rows = fy_seq_empty;
	live = fyai_remote_project_sessions(dir, gb);
	w->projects = fy_append(gb, w->projects, fy_mapping(gb,
		"project", fy_value(gb, rel), "sessions", rows, "live", live));
	return fy_is_valid(w->projects) ? 0 : -1;
}

/*
 * Scan the fleet tree rooted at cwd into @w using root `fleet/scan` config.
 */
static int fleet_collect(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 size_t turns, struct fleet_walk *w)
{
	fy_generic config = fy_invalid, scan, exclude;
	char *cwd, *root = NULL;
	size_t i;
	int rc = -1;

	w->gb = gb;
	w->turns = turns;
	w->projects = fy_seq_empty;
	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, cwd, out, "fleet: cannot read the current directory");
	root = fyai_fleet_root_of(cwd);
	fyai_error_check(ctx, root, out, "fleet: no project holds '%s'", cwd);
	/* Fall back to defaults if root config is unreadable. */
	if (fyai_peek_arena_config(fy_sprintfa("%s/.fyai/arena", root), NULL, NULL,
				   gb, &config, NULL, NULL, NULL))
		config = fy_invalid;
	scan = fy_get(fy_get(config, "fleet", fy_invalid), "scan", fy_invalid);
	exclude = fy_get(scan, "exclude", fy_invalid);
	if (!fy_is_sequence(exclude)) {
		exclude = fy_seq_empty;
		for (i = 0; i < ARRAY_SIZE(fleet_exclude_default); i++)
			exclude = fy_append(gb, exclude,
					    fy_value(gb, fleet_exclude_default[i]));
	}
	w->root = root;
	rc = fyai_fleet_scan(root, (int)fy_get(scan, "depth",
					       (long long)DEFAULT_FLEET_SCAN_DEPTH),
			     exclude, fleet_found, w);
	fyai_error_check(ctx, rc >= 0, out, "fleet: cannot scan the root '%s'", root);
	fyai_error_check(ctx, !rc, out, "fleet: cannot keep the projects of the root");
out:
	free(root);
	free(cwd);
	w->root = NULL;
	return rc ? -1 : 0;
}

/* Find the entry in @rows with matching @branch, or return fy_invalid. */
static fy_generic fleet_row_of(fy_generic rows, fy_generic branch)
{
	fy_generic row;

	fy_foreach(row, rows)
		if (fy_equal(fy_get(row, "branch", fy_invalid), branch))
			return row;
	return fy_invalid;
}

/*
 * Build a list row for one session in @name: prefer live server state if
 * present, else derive "stored", "running" (live lock owner), or "lost" (dead
 * lock owner with pending work).
 */
static fy_generic fleet_list_row(struct fy_generic_builder *gb, fy_generic name,
				 fy_generic row, fy_generic live)
{
	fy_generic owner = fy_get(row, "owner", fy_invalid), out;
	fy_generic lineage = fy_get(row, "fleet", fy_invalid);
	fy_generic task = fy_get(lineage, "task", fy_invalid);
	fy_generic parent = fy_get(lineage, "parent", fy_invalid);
	const char *state;

	if (fy_is_valid(live)) {
		out = fy_mapping(gb, "project", name, "branch", fy_get(row, "branch", ""),
				 "state", fyai_remote_live_state(live),
				 "attached", fy_get(live, "attached", false),
				 "pid", fy_get(live, "pid", 0LL));
	} else {
		state = !fy_is_valid(owner) ? "stored" :
			fyai_pending_owner_alive(owner) ? "running" : "lost";
		out = fy_mapping(gb, "project", name, "branch", fy_get(row, "branch", ""),
				 "state", state, "attached", false);
		if (!strcmp(state, "running"))
			out = fy_assoc(gb, out, "pid", fy_get(owner, "pid", 0LL));
	}
	if (fy_is_int(task))
		out = fy_assoc(gb, out, "task", task);
	if (fy_is_string(fy_get(parent, "branch", fy_invalid)))
		out = fy_assoc(gb, out, "parent", fy_stringf(gb, "%s:%s",
			fy_get(parent, "project", "."), fy_get(parent, "branch", "")));
	return out;
}

int fyai_fleet_rows_list(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 fy_generic *rowsp)
{
	static const char *const words[] = { "list", NULL };
	struct fleet_walk w;
	fy_generic project, name, row, live, out = fy_seq_empty;

	if (fleet_arena_open(ctx))
		return fleet_child(ctx, gb, words, rowsp);
	if (fleet_collect(ctx, gb, 0, &w))
		return -1;
	fy_foreach(project, w.projects) {
		name = fy_get(project, "project", fy_invalid);
		fy_foreach(row, fy_get(project, "sessions", fy_seq_empty))
			out = fy_append(gb, out, fleet_list_row(gb, name, row,
				fleet_row_of(fy_get(project, "live", fy_seq_empty),
					     fy_get(row, "branch", fy_invalid))));
		/* Include live sessions that have not published a turn yet. */
		fy_foreach(live, fy_get(project, "live", fy_seq_empty)) {
			if (fy_is_valid(fleet_row_of(fy_get(project, "sessions",
							    fy_seq_empty),
						     fy_get(live, "branch", fy_invalid))))
				continue;
			out = fy_append(gb, out, fleet_list_row(gb, name,
				fy_mapping(gb, "branch", fy_get(live, "branch", "")),
				live));
		}
	}
	*rowsp = out;
	return fy_is_valid(out) ? 0 : -1;
}

int fyai_fleet_rows_scan(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 fy_generic *rowsp)
{
	static const char *const words[] = { "scan", NULL };
	struct fleet_walk w;
	fy_generic project, out = fy_seq_empty;

	if (fleet_arena_open(ctx))
		return fleet_child(ctx, gb, words, rowsp);
	if (fleet_collect(ctx, gb, 0, &w))
		return -1;
	fy_foreach(project, w.projects)
		out = fy_append(gb, out, fy_mapping(gb,
			"project", fy_get(project, "project", fy_invalid),
			"sessions", (long long)fy_len(fy_get(project, "sessions",
							   fy_seq_empty)),
			"live", (long long)fy_len(fy_get(project, "live",
						       fy_seq_empty))));
	*rowsp = out;
	return fy_is_valid(out) ? 0 : -1;
}

/* Flatten task maps from root session @rows into task row mappings. */
static fy_generic fleet_task_rows(struct fy_generic_builder *gb, fy_generic rows)
{
	fy_generic row, key, task, out = fy_seq_empty;

	fy_foreach(row, rows)
		fy_foreach_key_value(key, task, fy_get(row, "tasks", fy_map_empty))
			out = fy_append(gb, out, fy_mapping(gb,
				"id", strtoll(fy_castp(&key, "0"), NULL, 10),
				"session", fy_get(row, "branch", ""),
				"name", fy_stringf(gb, "%s:%s",
						   fy_get(task, "project", "."),
						   fy_get(task, "branch", "")),
				"state", fy_get(task, "state", "open"),
				"text", fy_get(task, "text", "")));
	return out;
}

int fyai_fleet_rows_tasks(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			  fy_generic *rowsp)
{
	static const char *const words[] = { "tasks", NULL };
	fy_generic rows;
	char *cwd, *root = NULL;
	int rc = -1;

	if (fleet_arena_open(ctx))
		return fleet_child(ctx, gb, words, rowsp);
	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, cwd, out, "fleet: cannot read the current directory");
	root = fyai_fleet_root_of(cwd);
	fyai_error_check(ctx, root, out, "fleet: no project holds '%s'", cwd);
	rc = fyai_peek_sessions(fy_sprintfa("%s/.fyai/arena", root), gb, NULL, 0, &rows);
	fyai_error_check(ctx, !rc, out, "fleet: cannot read the root '%s'", root);
	*rowsp = fleet_task_rows(gb, rows);
	rc = fy_is_valid(*rowsp) ? 0 : -1;
out:
	free(root);
	free(cwd);
	return rc;
}

/* Search limits: max whitespace-separated terms and snippet context radius. */
#define FLEET_QUERY_WORDS	8
#define FLEET_SNIPPET		80

struct fleet_query {
	char *words[FLEET_QUERY_WORDS];
	size_t count;
	char *buf;
};

static void fleet_query_free(struct fleet_query *q)
{
	free(q->buf);
	memset(q, 0, sizeof(*q));
}

static int fleet_query_parse(struct fleet_query *q, const char *query)
{
	char *p, *save = NULL, *word;

	memset(q, 0, sizeof(*q));
	q->buf = strdup(query);
	if (!q->buf)
		return -1;
	for (p = q->buf; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	for (word = strtok_r(q->buf, " \t\n", &save); word && q->count < FLEET_QUERY_WORDS;
	     word = strtok_r(NULL, " \t\n", &save))
		q->words[q->count++] = word;
	return q->count ? 0 : -1;
}

/* Collect searchable {where, text} pairs for a session. */
static fy_generic fleet_search_fields(struct fy_generic_builder *gb, fy_generic project,
				      fy_generic row)
{
	fy_generic fields, item;

	fields = fy_sequence(gb,
		fy_mapping(gb, "where", "project", "text", project),
		fy_mapping(gb, "where", "branch", "text", fy_get(row, "branch", "")),
		fy_mapping(gb, "where", "description", "text",
			   fy_get(row, "description", "")));
	fy_foreach(item, fy_get(row, "texts", fy_seq_empty))
		fields = fy_append(gb, fields, fy_mapping(gb,
			"where", fy_get(item, "role", "message"),
			"text", fy_get(item, "text", "")));
	return fields;
}

/*
 * True if every term in @q matches at least one entry in @fields. Sets *@hitp
 * and *@wherep from the latest field matching q->words[0].
 */
static bool fleet_search_match(struct fy_generic_builder *gb,
			       const struct fleet_query *q, fy_generic fields,
			       fy_generic *hitp, fy_generic *wherep)
{
	fy_generic field, text;
	const char *s, *at;
	size_t i, start, len;
	bool found;

	for (i = 0; i < q->count; i++) {
		found = false;
		fy_foreach(field, fields) {
			text = fy_get(field, "text", fy_invalid);
			s = fy_castp(&text, "");
			at = strcasestr(s, q->words[i]);
			if (!at)
				continue;
			found = true;
			/* Overwrite so the newest matching field wins. */
			if (!i) {
				start = (size_t)(at - s) > FLEET_SNIPPET ?
					(size_t)(at - s) - FLEET_SNIPPET : 0;
				len = strlen(s + start);
				if (len > 2 * FLEET_SNIPPET + strlen(q->words[0]))
					len = 2 * FLEET_SNIPPET + strlen(q->words[0]);
				*hitp = fy_stringf(gb, "%s%.*s%s", start ? "..." : "",
						   (int)len, s + start,
						   s[start + len] ? "..." : "");
				*wherep = fy_get(field, "where", fy_invalid);
			}
		}
		if (!found)
			return false;
	}
	return true;
}

int fyai_fleet_rows_search(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			   const char *query, long long limit, fy_generic *rowsp)
{
	struct fleet_query q;
	struct fleet_walk w;
	fy_generic project, name, row, task, hit, where, out = fy_seq_empty;
	char count[24];
	const char *words[FLEET_CHILD_ARGS];
	size_t n = 0;
	int rc = -1;

	if (limit <= 0)
		limit = 20;
	if (limit > FYAI_FLEET_SEARCH_MAX)
		limit = FYAI_FLEET_SEARCH_MAX;
	if (fleet_arena_open(ctx)) {
		snprintf(count, sizeof(count), "%lld", limit);
		words[n++] = "search";
		words[n++] = "--limit";
		words[n++] = count;
		words[n++] = query;
		words[n] = NULL;
		return fleet_child(ctx, gb, words, rowsp);
	}
	fyai_error_check(ctx, !fleet_query_parse(&q, query), out_free,
			 "fleet: the query has no word");
	if (fleet_collect(ctx, gb, FYAI_FLEET_SEARCH_TURNS, &w))
		goto out_free;
	fy_foreach(project, w.projects) {
		name = fy_get(project, "project", fy_invalid);
		fy_foreach(row, fy_get(project, "sessions", fy_seq_empty)) {
			hit = where = fy_invalid;
			if ((long long)fy_len(out) < limit &&
			    fleet_search_match(gb, &q, fleet_search_fields(gb, name, row),
					       &hit, &where))
				out = fy_append(gb, out, fy_mapping(gb,
					"name", fy_stringf(gb, "%s:%s", fy_castp(&name, "."),
							   fy_get(row, "branch", "")),
					"kind", "session", "where", where, "text", hit));
			fy_foreach(task, fleet_task_rows(gb, fy_sequence(gb, row))) {
				hit = where = fy_invalid;
				if ((long long)fy_len(out) < limit &&
				    fleet_search_match(gb, &q, fy_sequence(gb,
						fy_mapping(gb, "where", "task", "text",
							   fy_get(task, "text", ""))),
						       &hit, &where))
					out = fy_append(gb, out, fy_mapping(gb,
						"name", fy_get(task, "name", ""),
						"kind", "task", "where",
						fy_stringf(gb, "task %lld of %s",
							   fy_get(task, "id", 0LL),
							   fy_get(task, "session", "")),
						"text", hit));
			}
		}
	}
	*rowsp = out;
	rc = fy_is_valid(out) ? 0 : -1;
out_free:
	fleet_query_free(&q);
	return rc;
}

/*
 * Split @name (`PROJECT:BRANCH` or local `BRANCH`) into *@dirp (NULL for cwd
 * project; caller frees) and *@branchp (borrows from @name).
 */
static int fleet_name(struct fyai_ctx *ctx, const char *name, char **dirp,
		      const char **branchp)
{
	char *cwd = getcwd(NULL, 0);
	int rc;

	fyai_error_check(ctx, cwd, err, "cannot read the current directory");
	rc = fyai_fleet_name_split(cwd, name, dirp, branchp);
	free(cwd);
	fyai_error_check(ctx, !rc, err, "'%s' names no branch", name);
	return 0;
err:
	return -1;
}

int fyai_fleet_rows_read(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 const char *name, long long turns, fy_generic *rowsp)
{
	const char *words[6], *branch;
	char count[24], *dir = NULL, *project = NULL;
	fy_generic rows;
	int rc = -1;

	if (turns <= 0)
		turns = 4;
	if (turns > FYAI_PEEK_TURNS_MAX)
		turns = FYAI_PEEK_TURNS_MAX;
	if (fleet_arena_open(ctx)) {
		snprintf(count, sizeof(count), "%lld", turns);
		words[0] = "read";
		words[1] = "--turns";
		words[2] = count;
		words[3] = name;
		words[4] = NULL;
		return fleet_child(ctx, gb, words, rowsp);
	}
	if (fleet_name(ctx, name, &dir, &branch))
		return -1;
	if (!dir) {
		dir = getcwd(NULL, 0);
		fyai_error_check(ctx, dir, out, "cannot read the current directory");
	}
	project = fyai_fleet_project_of(dir);
	fyai_error_check(ctx, project, out, "fleet: no project holds '%s'", dir);
	rc = fyai_peek_sessions(fy_sprintfa("%s/.fyai/arena", project), gb, branch,
				(size_t)turns, &rows);
	fyai_error_check(ctx, !rc, out, "fleet: cannot read the project '%s'", project);
	rc = -1;
	fyai_error_check(ctx, !fy_empty(rows), out, "fleet: no session '%s'", name);
	*rowsp = fy_get(fy_get_at(rows, 0), "texts", fy_seq_empty);
	rc = 0;
out:
	free(project);
	free(dir);
	return rc;
}

/* Branch task map: staged bstate, else last published, else empty. */
static fy_generic fleet_tasks_of(struct fyai_ctx *ctx)
{
	struct fyai_branch b;

	if (fy_is_mapping(ctx->bstate->tasks))
		return ctx->bstate->tasks;
	if (fy_is_valid(ctx->bstate->prev) && fyai_branch_decode(ctx->bstate->prev, &b) &&
	    fy_is_mapping(b.tasks))
		return b.tasks;
	return fy_map_empty;
}

/* Return one greater than the highest numeric task ID in @tasks. */
static long long fleet_task_next(fy_generic tasks)
{
	fy_generic key, value;
	long long id, top = 0;

	fy_foreach_key_value(key, value, tasks) {
		(void)value;
		id = strtoll(fy_castp(&key, "0"), NULL, 10);
		if (id > top)
			top = id;
	}
	return top + 1;
}

/*
 * Update staged bstate tasks; publish immediately if @publish is true and the
 * branch is already stored, otherwise defer to the next turn publish.
 */
static int fleet_tasks_publish(struct fyai_ctx *ctx, fy_generic tasks, bool publish)
{
	ctx->bstate->tasks = tasks;
	if (!publish || ctx->bstate->session_unstored)
		return 0;
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_CONFIG, NULL);
	return fyai_publish_state(ctx);
}

/* Derive a default `fleet/<slug>` branch name from @task in @gb. */
static const char *fleet_task_branch(struct fy_generic_builder *gb, const char *task)
{
	char slug[33];
	size_t n = 0;
	bool dash = false;
	const char *p;

	for (p = task; *p && n < sizeof(slug) - 1; p++) {
		if (isalnum((unsigned char)*p) && !((unsigned char)*p & 0x80)) {
			if (dash && n)
				slug[n++] = '-';
			if (n < sizeof(slug) - 1)
				slug[n++] = (char)tolower((unsigned char)*p);
			dash = false;
		} else {
			dash = true;
		}
	}
	slug[n] = '\0';
	return fy_gb_intern_string(gb, fy_sprintfa("fleet/%s", n ? slug : "task"));
}

int fyai_fleet_start(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		     const char *project, const char *task, const char *branch,
		     bool publish, fy_generic *resultp)
{
	fy_generic args = fy_invalid, tasks = fy_invalid;
	char *cwd = NULL, *root = NULL, *dir = NULL, *home = NULL;
	long long id = 0;
	int rc = -1;

	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, cwd, out, "cannot read the current directory");
	root = fyai_fleet_root_of(cwd);
	fyai_error_check(ctx, root, out, "fleet: no project holds '%s'", cwd);
	dir = realpath(strcmp(project, ".") ? fy_sprintfa("%s/%s", root, project) :
		       root, NULL);
	fyai_error_check(ctx, dir && !strncmp(dir, root, strlen(root)) &&
			 (!dir[strlen(root)] || dir[strlen(root)] == '/'), out,
			 "fleet: '%s' is no directory under the root '%s'", project,
			 root);
	if (!branch)
		branch = fleet_task_branch(gb, task);
	fyai_error_check(ctx, branch && fyai_branch_name_valid(branch), out,
			 "fleet: '%s' is not a valid branch", branch ? branch : "");
	/*
	 * Attach parent lineage when invoked from a session, and record a task
	 * when invoked from a root session (doc/fyai-fleet-sdd.md, section 9).
	 * Standalone CLI invocations have no session context.
	 */
	if (fleet_arena_open(ctx) && !ctx->cfg->transient) {
		home = fyai_fleet_project_of(cwd);
		args = fy_sequence(gb, "--fleet-parent",
			fy_stringf(gb, "%s:%s", home && strcmp(home, root) ?
				   home + strlen(root) + 1 : ".", fyai_ctx_branch(ctx)));
		if (home && !strcmp(home, root)) {
			tasks = fleet_tasks_of(ctx);
			id = fleet_task_next(tasks);
			args = fy_append(gb, fy_append(gb, args, fy_string("--fleet-task")),
					 fy_stringf(gb, "%lld", id));
			tasks = fy_assoc(ctx->arena->gb, tasks,
				fy_stringf(ctx->arena->gb, "%lld", id),
				fy_mapping(ctx->arena->gb, "project", project,
					   "branch", branch, "text", task,
					   "created", (long long)fyai_branch_timestamp(),
					   "state", "open"));
			fyai_error_check(ctx, fy_is_mapping(tasks), out,
					 "fleet: cannot keep the task");
		}
	}
	rc = fyai_remote_start(ctx, dir, branch, task, args);
	if (!rc && id && fleet_tasks_publish(ctx, tasks, publish))
		rc = -1;
	/* Subscribe root supervision to the newly started member. */
	if (!rc && id && fyai_fleet_watch_add(ctx, fy_sprintfa("%s:%s", project, branch)))
		fyai_warning(ctx, "fleet: '%s:%s' runs, but this session does not "
			     "hear of it", project, branch);
	if (!rc) {
		*resultp = fy_mapping(gb, "project", project, "branch", branch,
				      "name", fy_stringf(gb, "%s:%s", project, branch));
		if (id)
			*resultp = fy_assoc(gb, *resultp, "task", id);
	}
out:
	free(home);
	free(dir);
	free(root);
	free(cwd);
	return rc ? -1 : 0;
}

int fyai_fleet_task_mark(struct fyai_ctx *ctx, const char *id, const char *state,
			 bool publish)
{
	struct fy_generic_builder *gb = ctx->arena->gb;
	fy_generic tasks, task;

	fyai_error_check(ctx, fleet_arena_open(ctx), err,
			 "fleet: a task changes in the root session that keeps it");
	fyai_error_check(ctx, !strcmp(state, "open") || !strcmp(state, "done") ||
			 !strcmp(state, "dropped"), err,
			 "fleet: a task is open, done or dropped, not '%s'", state);
	tasks = fleet_tasks_of(ctx);
	task = fy_get(tasks, id, fy_invalid);
	fyai_error_check(ctx, fy_is_mapping(task), err,
			 "fleet: this session has no task %s", id);
	tasks = fy_assoc(gb, tasks, fy_value(gb, id),
			 fy_assoc(gb, task, "state", fy_value(gb, state)));
	fyai_error_check(ctx, fy_is_mapping(tasks), err, "fleet: cannot keep the task");
	return fleet_tasks_publish(ctx, tasks, publish);
err:
	return -1;
}

/* A scan stops at the second project: the root has one below it. */
static int fleet_root_found(const char *dir, void *data)
{
	int *count = data;

	(void)dir;
	return ++*count > 1;
}

bool fyai_fleet_is_root(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic scan = fy_get(fy_get(cfg->config_doc, "fleet", fy_invalid), "scan",
				 fy_invalid);
	char *cwd, *project = NULL, *root = NULL;
	int count = 0;

	if (cfg->fleet_tool)
		return cfg->fleet_tool > 0;
	cfg->fleet_tool = -1;
	cwd = getcwd(NULL, 0);
	if (cwd) {
		project = fyai_fleet_project_of(cwd);
		root = fyai_fleet_root_of(cwd);
	}
	if (project && root && !strcmp(project, root) &&
	    fyai_fleet_scan(root, (int)fy_get(scan, "depth",
					      (long long)DEFAULT_FLEET_SCAN_DEPTH),
			    fy_get(scan, "exclude", fy_seq_empty),
			    fleet_root_found, &count) > 0)
		cfg->fleet_tool = 1;
	free(root);
	free(project);
	free(cwd);
	return cfg->fleet_tool > 0;
}

fy_generic fyai_fleet_tasks_open(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic key, task, out = fy_map_empty;

	fy_foreach_key_value(key, task, fleet_tasks_of(ctx))
		if (fy_equal(fy_get(task, "state", fy_invalid), "open"))
			out = fy_assoc(gb, out, key, task);
	return out;
}

int fyai_fleet_revive(struct fyai_ctx *ctx, const char *name)
{
	const char *branch;
	char *dir = NULL;
	int rc;

	if (fleet_name(ctx, name, &dir, &branch))
		return -1;
	fyai_error_check(ctx, dir, err, "fleet: '%s' is not PROJECT:BRANCH", name);
	rc = fyai_remote_start(ctx, dir, branch, NULL, fy_invalid);
	free(dir);
	return rc;
err:
	return -1;
}

int fyai_fleet_supervise(struct fyai_ctx *ctx, const char *name,
			 const char *subtype, fy_generic fields)
{
	const char *branch;
	char *dir = NULL;
	int rc;

	if (fleet_name(ctx, name, &dir, &branch))
		return -1;
	rc = fyai_remote_supervise(ctx, dir, branch, subtype, fields);
	free(dir);
	return rc;
}

int fyai_fleet_close(struct fyai_ctx *ctx, const char *name, bool force,
		     bool *busyp)
{
	const char *branch;
	char *dir = NULL;
	int rc;

	if (fleet_name(ctx, name, &dir, &branch))
		return -1;
	rc = fyai_remote_session_close(ctx, dir, branch, force, busyp);
	free(dir);
	return rc;
}
