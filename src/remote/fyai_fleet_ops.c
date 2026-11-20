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
#include <sys/stat.h>
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

/* The directories that a scan does not go into when the configuration names none. */
static const char *const fleet_exclude_default[] = {
	"node_modules", "build*", ".cache",
};

/* The most words of a child command line, the program and its options included. */
#define FLEET_CHILD_ARGS	16

/* The arena of this process is open: the peeks run in a child. */
static bool fleet_arena_open(struct fyai_ctx *ctx)
{
	return ctx->arena->durable_allocator != NULL;
}

/*
 * Run `fleet SUB --output json REST` in a child with no arena and take its
 * rows; @words is SUB and REST. The options go before REST, which can end in
 * an argument that takes the rest of the line. The child is short; the read
 * waits for it.
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
	/* The cause of the child, with no prefix of its module: this report
	 * gives one. */
	while (err.len > 1 && (err.data[err.len - 2] == '\n' || err.data[err.len - 2] == ' '))
		err.data[--err.len - 1] = '\0';
	cause = err.data;
	if (!strncmp(cause, "remote: ", strlen("remote: ")))
		cause += strlen("remote: ");
	fyai_error_check(ctx, WIFEXITED(status) && !WEXITSTATUS(status), out,
			 "%s", *cause ? cause : fy_sprintfa("fleet: the scan %s %d",
				WIFEXITED(status) ? "exited with status" :
				"ended by signal", WIFEXITED(status) ?
				WEXITSTATUS(status) : WTERMSIG(status)));
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
	/* {project, dir, sessions, live} of each project: its stored rows and
	 * the rows of the record of its server. */
	fy_generic projects;
};

static int fleet_found(const char *dir, void *data)
{
	struct fleet_walk *w = data;
	struct fy_generic_builder *gb = w->gb;
	const char *rel = strcmp(dir, w->root) ? dir + strlen(w->root) + 1 : ".";
	fy_generic rows, live;

	/* A project that cannot be read is listed with no stored session. */
	if (fyai_peek_sessions(fy_sprintfa("%s/.fyai/arena", dir), gb, NULL,
			       w->turns, &rows))
		rows = fy_seq_empty;
	live = fyai_remote_project_sessions(dir, gb);
	w->projects = fy_append(gb, w->projects, fy_mapping(gb,
		"project", fy_value(gb, rel), "sessions", rows, "live", live));
	return fy_is_valid(w->projects) ? 0 : -1;
}

/*
 * Scan the root of the fleet of the current directory into @w, as `fleet/scan`
 * of the stored configuration of the root says.
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
	/* A root whose configuration cannot be read scans as the defaults say. */
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

/* The row of @rows whose branch is @branch, or fy_invalid. */
static fy_generic fleet_row_of(fy_generic rows, fy_generic branch)
{
	fy_generic row;

	fy_foreach(row, rows)
		if (fy_equal(fy_get(row, "branch", fy_invalid), branch))
			return row;
	return fy_invalid;
}

/*
 * The row of a session of @project for the list: a session that a server
 * lists has the state of the server; a session that no server lists is
 * stored, runs with no server, or was lost with work that a start restores.
 */
static fy_generic fleet_list_row(struct fy_generic_builder *gb, fy_generic name,
				 fy_generic row, fy_generic live)
{
	fy_generic owner = fy_get(row, "owner", fy_invalid), out;
	fy_generic lineage = fy_get(row, "fleet", fy_invalid);
	fy_generic task = fy_get(lineage, "task", fy_invalid);
	fy_generic parent = fy_get(lineage, "parent", fy_invalid);
	fy_generic title = fy_get(row, "description", fy_invalid);
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
	if (fy_is_string(title) && !fy_empty(title))
		out = fy_assoc(gb, out, "title", title);
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
		/* A live session with no stored exchange yet. */
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

fy_generic fyai_fleet_rows_in(struct fy_generic_builder *gb, fy_generic rows,
			      const char *project)
{
	fy_generic row, out = fy_seq_empty;
	const char *name;
	size_t len;

	if (!project || !*project)
		return rows;
	len = strlen(project);
	while (len > 1 && project[len - 1] == '/')
		len--;
	fy_foreach(row, rows) {
		name = fy_get(row, "project", ".");
		if (!strncmp(name, project, len) &&
		    (!name[len] || name[len] == '/'))
			out = fy_append(gb, out, row);
	}
	return out;
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

/* The task rows of the stored @rows of the root. */
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

/* The words of a query: lower case, at most this many. */
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

/* The fields of a session that a search reads: {where, text}. */
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
 * Whether the @fields hold each word of @q. The part of the last field that
 * holds the first word, around it, goes to *@hitp, and its field to *@wherep.
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
			/* The newest message that holds the first word. */
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
			/* The tasks that the root sessions keep. */
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
 * The project directory and the branch of the session @name: `PROJECT:BRANCH`
 * of the fleet, or `BRANCH` of this project. *@dirp is NULL for this project;
 * the caller frees it.
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

/* The tasks of this branch, a mapping by id: staged, else stored, else none. */
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

/* The id of the next task: one above the highest id of @tasks. */
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
 * Record @tasks as the tasks of this branch: now with @publish when the branch
 * is stored, else at the next publish, which a turn makes.
 */
static int fleet_tasks_publish(struct fyai_ctx *ctx, fy_generic tasks, bool publish)
{
	ctx->bstate->tasks = tasks;
	if (!publish || ctx->bstate->session_unstored)
		return 0;
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_CONFIG, NULL);
	return fyai_publish_state(ctx);
}

/* A branch for a task with no branch named: `fleet/` and its first words. */
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

/* Whether @path under @root, both real, is @root or below it. */
static bool fleet_under(const char *path, const char *root)
{
	size_t len = strlen(root);

	return !strncmp(path, root, len) && (!path[len] || path[len] == '/');
}

/* Whether @project is a relative path of plain names, no `.`, `..` or empty. */
static bool fleet_project_valid(const char *project)
{
	const char *seg = project, *end;
	size_t len;

	if (!*project || *project == '/')
		return false;
	for (;; seg = end + 1) {
		end = strchr(seg, '/');
		len = end ? (size_t)(end - seg) : strlen(seg);
		if (!len || (len == 1 && *seg == '.') ||
		    (len == 2 && seg[0] == '.' && seg[1] == '.') ||
		    (len == 5 && !strncmp(seg, ".fyai", 5)))
			return false;
		if (!end)
			return true;
	}
}

/*
 * Make the directory @project below @root, each name of it in turn, and take
 * its real path in *@dirp. A name that is a link out of the tree stops it
 * before anything is made through it.
 */
static int fleet_make_dir(struct fyai_ctx *ctx, const char *root,
			  const char *project, char **dirp)
{
	char *cur = NULL, *real = NULL, *next = NULL;
	const char *seg = project, *end;
	struct stat st;
	size_t len;
	bool made;
	int rc = -1;

	cur = strdup(root);
	fyai_error_check(ctx, cur, out, "fleet: out of memory");
	for (;; seg = end + 1) {
		end = strchr(seg, '/');
		len = end ? (size_t)(end - seg) : strlen(seg);
		if (asprintf(&next, "%s/%.*s", cur, (int)len, seg) < 0)
			next = NULL;
		fyai_error_check(ctx, next, out, "fleet: out of memory");
		free(cur);
		cur = next;
		next = NULL;
		made = !mkdir(cur, 0777) ||
		       (errno == EEXIST && !stat(cur, &st) && S_ISDIR(st.st_mode));
		fyai_error_check(ctx, made, out,
				 "fleet: cannot make the directory %s: %s", cur,
				 strerror(errno));
		real = realpath(cur, NULL);
		fyai_error_check(ctx, real && fleet_under(real, root), out,
				 "fleet: '%s' leaves the root '%s'", project,
				 root);
		free(real);
		real = NULL;
		if (!end)
			break;
	}
	*dirp = realpath(cur, NULL);
	fyai_error_check(ctx, *dirp, out, "fleet: cannot resolve %s: %s", cur,
			 strerror(errno));
	rc = 0;
out:
	free(real);
	free(next);
	free(cur);
	return rc;
}

int fyai_fleet_create(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		      const char *project, const char *task, const char *branch,
		      bool default_config, bool publish, fy_generic *resultp)
{
	fy_generic started = fy_invalid, config;
	char *cwd = NULL, *root = NULL, *dir = NULL, *home = NULL;
	bool created = false;
	int rc = -1;

	fyai_error_check(ctx, fleet_project_valid(project), err,
			 "fleet: '%s' is no project name: give a path of plain "
			 "names below the root", project);
	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, cwd, err, "cannot read the current directory");
	root = fyai_fleet_root_of(cwd);
	fyai_error_check(ctx, root, err, "fleet: no project holds '%s'", cwd);
	if (fleet_make_dir(ctx, root, project, &dir))
		goto err;
	/* A directory that holds a project of its own is kept as it is. */
	home = fyai_fleet_project_of(dir);
	if (!home || strcmp(home, dir)) {
		/* The project is no root: it does not take fleet/root. */
		config = default_config ? fy_invalid :
			 fyai_config_fleet_root(gb, ctx->cfg->config_doc,
						 false);
		if (!default_config && !fy_is_valid(config)) {
			fyai_error(ctx, "fleet: cannot take the configuration "
				       "of this session");
			goto err;
		}
		if (fyai_arena_create(ctx, dir, config))
			goto err;
		created = true;
	}
	if (task) {
		if (fyai_fleet_start(ctx, gb, project, task, branch, publish,
				     &started))
			goto err;
		*resultp = fy_assoc(gb, started, "created", created);
	} else {
		*resultp = fy_mapping(gb, "project", project, "created",
				      created);
	}
	rc = 0;
err:
	free(home);
	free(dir);
	free(root);
	free(cwd);
	return rc;
}

int fyai_fleet_start(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		     const char *project, const char *task, const char *branch,
		     bool publish, fy_generic *resultp)
{
	fy_generic args = fy_invalid, tasks = fy_invalid;
	char *cwd = NULL, *root = NULL, *dir = NULL, *home = NULL;
	long long id = 0;
	bool queued = false;
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
	 * A session that starts one is its parent; a session of the root
	 * keeps a task for it (doc/fyai-fleet-sdd.md, section 9). A command
	 * line has no session: the new one has no parent and no task.
	 */
	if (fleet_arena_open(ctx) && !ctx->cfg->transient) {
		home = fyai_fleet_project_of(cwd);
		args = fy_sequence(gb, "--fleet-parent",
			fy_stringf(gb, "%s:%s", home && strcmp(home, root) ?
				   home + strlen(root) + 1 : ".", fyai_ctx_branch(ctx)));
		if (home && !strcmp(home, root)) {
			/* Above fleet/max_live the task waits for a place. */
			queued = !fyai_fleet_has_room(ctx);
			tasks = fleet_tasks_of(ctx);
			id = fleet_task_next(tasks);
			args = fy_append(gb, fy_append(gb, args, fy_string("--fleet-task")),
					 fy_stringf(gb, "%lld", id));
			tasks = fy_assoc(ctx->arena->gb, tasks,
				fy_stringf(ctx->arena->gb, "%lld", id),
				fy_mapping(ctx->arena->gb, "project", project,
					   "branch", branch, "text", task,
					   "created", (long long)fyai_branch_timestamp(),
					   "state", queued ? "queued" : "open"));
			fyai_error_check(ctx, fy_is_mapping(tasks), out,
					 "fleet: cannot keep the task");
		}
	}
	rc = queued ? 0 : fyai_remote_start(ctx, dir, branch, task, args);
	if (!rc && id && fleet_tasks_publish(ctx, tasks, publish))
		rc = -1;
	/* The root session hears of the member as it changes. */
	if (!rc && id && !queued && fyai_fleet_watch_add(ctx, fy_sprintfa("%s:%s", project, branch)))
		fyai_warning(ctx, "fleet: '%s:%s' runs, but this session does not "
			     "hear of it", project, branch);
	if (!rc) {
		*resultp = fy_mapping(gb, "project", project, "branch", branch,
				      "name", fy_stringf(gb, "%s:%s", project, branch));
		if (id)
			*resultp = fy_assoc(gb, *resultp, "task", id);
		if (queued)
			*resultp = fy_assoc(gb, *resultp, "queued", true);
	}
out:
	free(home);
	free(dir);
	free(root);
	free(cwd);
	return rc ? -1 : 0;
}

int fyai_fleet_adopt(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		     const char *name, const char *text, bool publish,
		     long long *idp)
{
	fy_generic tasks;
	const char *full, *colon, *project, *home_name;
	char *cwd = NULL, *root = NULL, *home = NULL;
	long long id;
	int rc = -1;

	fyai_error_check(ctx, fyai_fleet_is_root(ctx) && fleet_arena_open(ctx) &&
			 !ctx->cfg->transient, out,
			 "fleet: only a root session adopts a session");
	fyai_error_check(ctx, fyai_fleet_has_room(ctx), out,
			 "fleet: the most members that fleet/max_live allows run; "
			 "wait for one to end");
	full = fyai_fleet_name_full(ctx, gb, name);
	if (!full)
		goto out;
	colon = strchr(full, ':');
	project = fy_gb_intern_string(gb, fy_sprintfa("%.*s", (int)(colon - full), full));
	cwd = getcwd(NULL, 0);
	root = cwd ? fyai_fleet_root_of(cwd) : NULL;
	home = cwd ? fyai_fleet_project_of(cwd) : NULL;
	fyai_error_check(ctx, root && home, out, "fleet: no project holds this session");
	home_name = fy_gb_intern_string(gb, fy_sprintfa("%s:%s", strcmp(home, root) ?
			home + strlen(root) + 1 : ".", fyai_ctx_branch(ctx)));
	tasks = fleet_tasks_of(ctx);
	id = fleet_task_next(tasks);
	/* The session takes the lineage first: a branch with no live session fails. */
	if (fyai_fleet_supervise(ctx, full, "fyai_session_lineage",
				 fy_mapping(gb, "parent", home_name, "task", id)))
		goto out;
	tasks = fy_assoc(ctx->arena->gb, tasks, fy_stringf(ctx->arena->gb, "%lld", id),
		fy_mapping(ctx->arena->gb, "project", project, "branch", colon + 1,
			   "text", text && *text ? text : "Adopted session",
			   "created", (long long)fyai_branch_timestamp(),
			   "state", "open"));
	fyai_error_check(ctx, fy_is_mapping(tasks), out, "fleet: cannot keep the task");
	rc = fleet_tasks_publish(ctx, tasks, publish);
	if (!rc && fyai_fleet_watch_add(ctx, full))
		fyai_warning(ctx, "fleet: '%s' is adopted, but this session does not "
			     "hear of it", full);
	if (!rc && idp)
		*idp = id;
out:
	free(home);
	free(root);
	free(cwd);
	return rc ? -1 : 0;
}

int fyai_fleet_dequeue(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb = ctx->arena->gb, *tgb = fyai_ctx_transient_gb(ctx);
	fy_generic tasks, key, task, pick = fy_invalid, args, mark;
	const char *project, *branch, *text, *name, *id = NULL;
	char *cwd = NULL, *root = NULL, *dir = NULL, *home = NULL, *cause;
	long long best = 0, n;
	int started = 0, rc;

	if (!fyai_fleet_is_root(ctx) || !fleet_arena_open(ctx) || ctx->cfg->transient)
		return 0;
	while (fyai_fleet_has_room(ctx)) {
		tasks = fleet_tasks_of(ctx);
		pick = fy_invalid;
		best = 0;
		/* The oldest queued task: the lowest id. */
		fy_foreach_key_value(key, task, tasks) {
			if (!fy_equal(fy_get(task, "state", fy_invalid), "queued"))
				continue;
			n = strtoll(fy_castp(&key, "0"), NULL, 10);
			if (!best || n < best) {
				best = n;
				pick = task;
				id = fy_gb_intern_string(tgb, fy_castp(&key, ""));
			}
		}
		if (!best)
			break;
		project = fy_get(pick, "project", ".");
		branch = fy_get(pick, "branch", "");
		text = fy_get(pick, "text", "");
		name = fy_gb_intern_string(tgb, fy_sprintfa("%s:%s", project, branch));
		cwd = getcwd(NULL, 0);
		root = cwd ? fyai_fleet_root_of(cwd) : NULL;
		home = cwd ? fyai_fleet_project_of(cwd) : NULL;
		dir = root ? realpath(strcmp(project, ".") ?
				      fy_sprintfa("%s/%s", root, project) : root, NULL) : NULL;
		rc = -1;
		if (dir && home) {
			args = fy_sequence(tgb, "--fleet-parent",
				fy_stringf(tgb, "%s:%s", strcmp(home, root) ?
					   home + strlen(root) + 1 : ".",
					   fyai_ctx_branch(ctx)),
				"--fleet-task", fy_value(tgb, id));
			rc = fyai_remote_start(ctx, dir, branch, text, args);
		}
		free(dir);
		free(home);
		free(root);
		free(cwd);
		cwd = root = dir = home = NULL;
		/* A task that cannot start is dropped: the loop must end. */
		mark = fy_assoc(gb, pick, "state", fy_value(gb, rc ? "dropped" : "open"));
		tasks = fy_assoc(gb, tasks, fy_value(gb, id), mark);
		if (!fy_is_mapping(tasks) || fleet_tasks_publish(ctx, tasks, false))
			break;
		if (rc) {
			cause = fyai_diag_string(fyai_ctx_diag(ctx));
			fyai_diag_tracef("fleet", "'%s' did not start from the queue: %s",
					 name, cause && *cause ? cause : "no cause");
			free(cause);
			fyai_diag_reset(fyai_ctx_diag(ctx));
			(void)fyai_event_injectf(ctx->fleet->waitq, "[session '%s' dropped: "
				"its queued task could not start]", name);
			continue;
		}
		if (fyai_fleet_watch_add(ctx, name))
			fyai_warning(ctx, "fleet: '%s' runs, but this session does not "
				     "hear of it", name);
		(void)fyai_event_injectf(ctx->fleet->waitq, "[session '%s' started: a "
			"place became free for its queued task]", name);
		started++;
	}
	return started;
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

bool fyai_fleet_has_room(struct fyai_ctx *ctx)
{
	long long max = fy_get(fy_get(ctx->cfg->config_doc, "fleet", fy_invalid),
			       "max_live", (long long)FYAI_FLEET_MAX_LIVE);

	return max <= 0 || (long long)fyai_fleet_watch_live(ctx) < max;
}

bool fyai_fleet_is_root(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic fleet;
	char *cwd, *project = NULL, *root = NULL;

	if (cfg->fleet_tool)
		return cfg->fleet_tool > 0;
	cfg->fleet_tool = -1;
	fleet = fy_get(cfg->config_doc, "fleet", fy_invalid);
	if (!fy_get(fleet, "root", false))
		return false;
	cwd = getcwd(NULL, 0);
	if (cwd) {
		project = fyai_fleet_project_of(cwd);
		root = fyai_fleet_root_of(cwd);
	}
	if (project && root && !strcmp(project, root))
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
	rc = fyai_remote_serve_ensure(ctx, dir) ? -1 :
	     fyai_remote_start(ctx, dir, branch, NULL, fy_invalid);
	free(dir);
	return rc;
err:
	return -1;
}

const char *fyai_fleet_name_full(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
				 const char *name)
{
	const char *branch, *full = NULL;
	char *dir = NULL, *cwd = NULL, *root = NULL, *project = NULL;

	if (fleet_name(ctx, name, &dir, &branch))
		return NULL;
	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, cwd, out, "cannot read the current directory");
	root = fyai_fleet_root_of(cwd);
	project = fyai_fleet_project_of(dir ? dir : cwd);
	fyai_error_check(ctx, root && project, out, "fleet: no project holds '%s'",
			 dir ? dir : cwd);
	full = fy_gb_intern_string(gb, fy_sprintfa("%s:%s", strcmp(project, root) ?
						   project + strlen(root) + 1 : ".",
						   branch));
out:
	free(project);
	free(root);
	free(cwd);
	free(dir);
	return full;
}

/* The row of the list @rows for the session @full (PROJECT:BRANCH). */
static fy_generic fleet_row_find(fy_generic rows, const char *full)
{
	fy_generic row;

	fy_foreach(row, rows)
		if (!strcmp(fy_sprintfa("%s:%s", fy_get(row, "project", "."),
					fy_get(row, "branch", "")), full))
			return row;
	return fy_invalid;
}

/*
 * The session of the project @project that a jump takes: its branch `main`,
 * else the first row of the project. NULL when the list has no such project.
 */
static const char *fleet_project_session(struct fy_generic_builder *gb,
					 fy_generic rows, const char *project)
{
	const char *branch = NULL;
	fy_generic row;

	fy_foreach(row, rows) {
		if (strcmp(fy_get(row, "project", "."), project))
			continue;
		if (!branch)
			branch = fy_get(row, "branch", "");
		if (fy_equal(fy_get(row, "branch"), "main")) {
			branch = "main";
			break;
		}
	}
	if (!branch)
		return NULL;
	return fy_gb_intern_string(gb, fy_sprintfa("%s:%s", project, branch));
}

const char *fyai_fleet_jump_name(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb, const char *name)
{
	const char *full, *project;
	fy_generic rows;

	full = fyai_fleet_name_full(ctx, gb, name);
	if (!full || strchr(name, ':') || fyai_fleet_rows_list(ctx, gb, &rows))
		return full;
	if (fy_is_valid(fleet_row_find(rows, full)))
		return full;
	project = fleet_project_session(gb, rows, name);
	return project ? project : full;
}

int fyai_fleet_attach(struct fyai_ctx *ctx, const char *name, const char *ret,
		      bool *watchp)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic rows, row, state;
	const char *full, *branch;
	bool attached;
	char *dir = NULL;
	int rc = -1;

	full = fyai_fleet_jump_name(ctx, gb, name);
	if (!full || fleet_name(ctx, full, &dir, &branch))
		return -1;
	if (fyai_fleet_rows_list(ctx, gb, &rows))
		goto out;
	row = fleet_row_find(rows, full);
	fyai_error_check(ctx, fy_is_valid(row), out, "fleet: no session '%s'", full);
	state = fy_get(row, "state", fy_invalid);
	attached = fy_get(row, "attached", false);
	/* A session that no process runs starts at the server of its project. */
	if (fy_any_equal(state, "stored", "lost") && fyai_fleet_revive(ctx, full))
		goto out;
	*watchp = fy_any_equal(state, "idle", "running", "waiting") && attached;
	free(cfg->attach_branch);
	free(cfg->attach_dir);
	free(cfg->attach_return);
	cfg->attach_branch = strdup(branch);
	cfg->attach_dir = dir ? strdup(dir) : NULL;
	cfg->attach_return = ret ? strdup(ret) : NULL;
	cfg->attach_watch = *watchp;
	fyai_error_check(ctx, cfg->attach_branch && (!dir || cfg->attach_dir) &&
			 (!ret || cfg->attach_return), out,
			 "fleet: cannot keep the session to join");
	rc = 0;
out:
	free(dir);
	return rc;
}

/* Whether the session @name is stored: no process runs it. */
static bool fleet_session_stored(struct fyai_ctx *ctx, const char *name)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic rows, row;
	const char *full;

	full = fyai_fleet_name_full(ctx, gb, name);
	if (!full || fyai_fleet_rows_list(ctx, gb, &rows))
		return false;
	row = fleet_row_find(rows, full);
	return fy_is_valid(row) &&
	       fy_any_equal(fy_get(row, "state", fy_invalid), "stored", "lost");
}

/*
 * The root session takes the member @name, which it woke with @text: it keeps
 * the open task of the member, or adopts the member under a new one, and
 * supervises it.
 */
static int fleet_member_take(struct fyai_ctx *ctx, const char *name, const char *text)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic key, task;
	const char *full = fyai_fleet_name_full(ctx, gb, name);

	if (!full)
		return -1;
	fy_foreach_key_value(key, task, fyai_fleet_tasks_open(ctx)) {
		(void)key;
		if (!strcmp(fy_sprintfa("%s:%s", fy_get(task, "project", "."),
					fy_get(task, "branch", "")), full))
			return fyai_fleet_watch_add(ctx, full);
	}
	return fyai_fleet_adopt(ctx, gb, full, text, false, NULL);
}

int fyai_fleet_supervise(struct fyai_ctx *ctx, const char *name,
			 const char *subtype, fy_generic fields)
{
	const char *branch;
	char *dir = NULL;
	bool send;
	int rc;

	if (fleet_name(ctx, name, &dir, &branch))
		return -1;
	send = !strcmp(subtype, "fyai_session_send");
	if (send && fleet_session_stored(ctx, name))
		rc = fyai_remote_serve_ensure(ctx, dir) ? -1 :
		     fyai_remote_start(ctx, dir, branch,
				       fy_get(fields, "text", ""), fy_invalid);
	else
		rc = fyai_remote_supervise(ctx, dir, branch, subtype, fields);
	/* A root session hears of the member it sent to, as of one it started. */
	if (!rc && send && fyai_fleet_is_root(ctx) &&
	    fleet_member_take(ctx, name, fy_get(fields, "text", "")))
		fyai_warning(ctx, "fleet: '%s' got the message, but this session "
			     "does not hear of it", name);
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
