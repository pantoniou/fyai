/*
 * fyai_orphan_linux.c - processes that a tool leaves running
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "fyai_host_backend.h"
#include "fyai.h"
#include "fyai_cover.h"
#include "fyai_event.h"
#include "fyai_orphan.h"

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef FYAI_HOST_BACKEND_LINUX
#include <sys/prctl.h>
#include <sys/syscall.h>

/* Most processes that the scope walks below its roots. */
#define ORPHAN_WALK_MAX 4096

struct fyai_orphan {
	struct fyai_orphan *next;
	int pidfd;
	pid_t pid;
};

/* Set once this process is in a PID namespace that a scope holds. */
static bool orphan_namespaced;

void fyai_orphan_subreaper(void)
{
#ifdef PR_SET_CHILD_SUBREAPER
	(void)prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#endif
}

void fyai_orphan_namespaced(void)
{
	orphan_namespaced = true;
}

/* The start time of @pid in clock ticks since boot, or 0. */
/* The text after the first @count blank separated fields of @s, or NULL when it has fewer. */
static const char *orphan_skip_fields(const char *s, int count)
{
	while (count-- > 0) {
		s += strcspn(s, " ");
		if (!*s)
			return NULL;
		s += strspn(s, " ");
	}
	return *s ? s : NULL;
}

static unsigned long long orphan_start(pid_t pid, char *state)
{
	char path[64], buf[1024], *p, *end;
	const char *q;
	unsigned long long start = 0;
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	/* The name is in parentheses and can hold blanks: read after the last one. */
	p = strrchr(buf, ')');
	if (!p || p[1] != ' ')
		return 0;
	if (state)
		*state = p[2];
	/* Field 3 is the state and field 22 the start time: skip the 19 fields from 3 to 21. */
	q = orphan_skip_fields(p + 2, 19);
	if (!q)
		return 0;
	start = strtoull(q, &end, 10);
	return end == q ? 0 : start;
}

/* Append the pids that the children files of the threads of @pid name. */
static size_t orphan_children_of(pid_t pid, pid_t *out, size_t max)
{
	char path[320], buf[1024];
	struct dirent *de;
	size_t n = 0;
	DIR *dir;
	ssize_t got;
	char *p, *end;
	long v;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
	dir = opendir(path);
	if (!dir)
		return 0;
	while ((de = readdir(dir))) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%d/task/%s/children",
			 (int)pid, de->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		got = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (got <= 0)
			continue;
		buf[got] = '\0';
		for (p = buf; *p && n < max; p = end) {
			v = strtol(p, &end, 10);
			if (end == p)
				break;
			if (v > 0)
				out[n++] = (pid_t)v;
		}
	}
	closedir(dir);
	return n;
}

fy_generic fyai_orphan_children(struct fy_generic_builder *gb)
{
	fy_generic seq = fy_sequence(gb);
	pid_t kids[256];
	unsigned long long start;
	size_t n, i;
	char state;

	if (orphan_namespaced)
		return seq;
	n = orphan_children_of(getpid(), kids, ARRAY_SIZE(kids));
	for (i = 0; i < n; i++) {
		start = orphan_start(kids[i], &state);
		/* A zombie has ended: nothing is left to stop. */
		if (!start || state == 'Z')
			continue;
		seq = fy_append(gb, seq, fy_mapping(gb, "pid", (long long)kids[i],
						    "start", (long long)start));
	}
	return seq;
}

void fyai_orphan_name(pid_t pid, char *name, size_t size)
{
	char path[64];
	ssize_t n;
	int fd;

	name[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	n = read(fd, name, size - 1);
	close(fd);
	if (n < 0)
		n = 0;
	name[n] = '\0';
	if (n && name[n - 1] == '\n')
		name[n - 1] = '\0';
}

void fyai_orphan_label(pid_t pid, char *label, size_t size)
{
	static const char key[] = "FYAI_DAEMON=";
	char path[64], env[4096], *p, *end;
	ssize_t n;
	int fd;

	label[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/environ", (int)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	n = read(fd, env, sizeof(env) - 1);
	close(fd);
	if (n <= 0)
		return;
	env[n] = '\0';
	/* The entries end in NUL; the one that the read cut short is not whole. */
	for (p = env; p < env + n; p = end + 1) {
		end = memchr(p, '\0', (size_t)(env + n - p));
		if (!end)
			break;
		if (!strncmp(p, key, sizeof(key) - 1)) {
			snprintf(label, size, "%s", p + sizeof(key) - 1);
			return;
		}
	}
}

fy_generic fyai_orphan_note(struct fy_generic_builder *gb, const char *until)
{
	char names[256] = "", comm[32];
	pid_t kids[256];
	size_t n, i, live = 0, used = 0;
	char state;

	n = orphan_children_of(getpid(), kids, ARRAY_SIZE(kids));
	for (i = 0; i < n; i++) {
		if (!orphan_start(kids[i], &state) || state == 'Z')
			continue;
		live++;
		fyai_orphan_name(kids[i], comm, sizeof(comm));
		if (used + strlen(comm) + 3 < sizeof(names))
			used += (size_t)snprintf(names + used,
						 sizeof(names) - used, "%s%s",
						 used ? ", " : "", comm);
	}
	if (!live)
		return fy_invalid;
	return fy_stringf(gb, "[%zu daemon%s still running: %s. "
			  "%s]", live, live == 1 ? "" : "s", names, until);
}

static int orphan_pidfd_open(pid_t pid)
{
#ifdef SYS_pidfd_open
	return (int)syscall(SYS_pidfd_open, pid, 0);
#else
	(void)pid;
	errno = ENOSYS;
	return -1;
#endif
}

static int orphan_signal(int pidfd, int sig)
{
#ifdef SYS_pidfd_send_signal
	return (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
#else
	(void)pidfd;
	(void)sig;
	errno = ENOSYS;
	return -1;
#endif
}

/* Keep @pid by a descriptor, if it is the process that started at @start. */
static void orphan_add(struct fyai_ctx *ctx, pid_t pid,
		       unsigned long long start)
{
	struct fyai_orphan *o;
	int fd;

	for (o = ctx->orphans; o; o = o->next)
		if (o->pid == pid)
			return;
	fd = orphan_pidfd_open(pid);
	if (fd < 0)
		return;
	/* The descriptor names the process that exists now: check it is the one. */
	if (start && orphan_start(pid, NULL) != start) {
		close(fd);
		return;
	}
	o = calloc(1, sizeof(*o));
	if (!o) {
		close(fd);
		fyai_error(ctx, "could not keep a process that a tool left");
		return;
	}
	o->pidfd = fd;
	o->pid = pid;
	o->next = ctx->orphans;
	ctx->orphans = o;
}

void fyai_orphan_track(struct fyai_ctx *ctx, fy_generic detached)
{
	fy_generic item;

	if (!fy_is_sequence(detached))
		return;
	fy_foreach(item, detached) {
		long long pid = fy_get(item, "pid", 0LL);
		long long start = fy_get(item, "start", 0LL);

		if (pid > 1 && start > 0)
			orphan_add(ctx, (pid_t)pid, (unsigned long long)start);
	}
}

fy_generic fyai_orphan_rows(const struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	const struct fyai_orphan *o;
	fy_generic rows = fy_sequence(gb);
	char name[32], label[64];

	for (o = ctx->orphans; o; o = o->next) {
		fyai_orphan_name(o->pid, name, sizeof(name));
		fyai_orphan_label(o->pid, label, sizeof(label));
		rows = fy_append(gb, rows, fy_mapping(gb, "pid", (long long)o->pid,
						      "name", fy_value(gb, name),
						      "label", fy_value(gb, label),
						      "held", "tracked"));
	}
	return rows;
}

size_t fyai_orphan_count(const struct fyai_ctx *ctx)
{
	const struct fyai_orphan *o;
	size_t n = 0;

	for (o = ctx->orphans; o; o = o->next)
		n++;
	return n;
}

static void orphan_free(struct fyai_orphan *o)
{
	close(o->pidfd);
	free(o);
}

/* Processes that a stop asked to end: SIGKILL follows when the grace time is over. */
struct orphan_stop {
	struct orphan_stop *next;
	struct fyai_event_source *timer;
	int *pidfds;
	size_t n;
};

static struct orphan_stop *orphan_stops;

static void orphan_stop_free(struct orphan_stop *s)
{
	size_t i;

	if (s->timer)
		fyai_event_source_remove(s->timer);
	for (i = 0; i < s->n; i++)
		close(s->pidfds[i]);
	free(s->pidfds);
	free(s);
}

static void orphan_stop_unlink(struct orphan_stop *s)
{
	struct orphan_stop **pp;

	for (pp = &orphan_stops; *pp; pp = &(*pp)->next)
		if (*pp == s) {
			*pp = s->next;
			return;
		}
}

static enum fyai_event_action orphan_stop_cb(const struct fyai_event *ev)
{
	struct orphan_stop *s = ev->userdata;
	size_t i;

	for (i = 0; i < s->n; i++)
		(void)orphan_signal(s->pidfds[i], SIGKILL);
	s->timer = NULL;
	orphan_stop_unlink(s);
	orphan_stop_free(s);
	return FYAIEA_STOP;
}

/* Whether the daemon @pid is the one that @which names: its label, or its pid. */
static bool orphan_matches(pid_t pid, const char *which)
{
	char label[64], num[24];

	snprintf(num, sizeof(num), "%d", (int)pid);
	if (!strcmp(num, which))
		return true;
	fyai_orphan_label(pid, label, sizeof(label));
	return *label && !strcmp(label, which);
}

size_t fyai_scope_stop(struct fyai_ctx *ctx, const char *which)
{
	struct fyai_orphan *o;
	struct orphan_stop *s;
	struct fyai_event_loop *el;
	pid_t members[64];
	size_t nmembers, i, n = 0;
	int fd;

	s = calloc(1, sizeof(*s));
	if (!s)
		return 0;
	s->pidfds = calloc(ARRAY_SIZE(members) * 2, sizeof(*s->pidfds));
	if (!s->pidfds) {
		free(s);
		return 0;
	}
	/* What the invocation tracks, and what the holder of the scope holds. */
	for (o = ctx->orphans; o && n < ARRAY_SIZE(members) * 2; o = o->next) {
		if (!orphan_matches(o->pid, which))
			continue;
		fd = orphan_pidfd_open(o->pid);
		if (fd >= 0)
			s->pidfds[n++] = fd;
	}
	nmembers = fyai_cover_members(&ctx->cover, members, ARRAY_SIZE(members));
	for (i = 0; i < nmembers && n < ARRAY_SIZE(members) * 2; i++) {
		if (!orphan_matches(members[i], which))
			continue;
		fd = orphan_pidfd_open(members[i]);
		if (fd >= 0)
			s->pidfds[n++] = fd;
	}
	s->n = n;
	if (!n) {
		orphan_stop_free(s);
		return 0;
	}
	for (i = 0; i < n; i++)
		(void)orphan_signal(s->pidfds[i], SIGTERM);
	el = fyai_ctx_loop(ctx);
	if (el && !fyai_event_add_timer(el, (int)ctx->cfg->sandbox_grace_ms, 0,
					orphan_stop_cb, s, &s->timer)) {
		s->next = orphan_stops;
		orphan_stops = s;
		return n;
	}
	/* No loop to wait on: end them now. */
	for (i = 0; i < n; i++)
		(void)orphan_signal(s->pidfds[i], SIGKILL);
	orphan_stop_free(s);
	return n;
}

static void orphan_stops_drop(void)
{
	struct orphan_stop *s;

	while ((s = orphan_stops)) {
		orphan_stops = s->next;
		orphan_stop_free(s);
	}
}

static void orphan_end_all(struct fyai_ctx *ctx, int sig)
{
	struct fyai_orphan *o;

	for (o = ctx->orphans; o; o = o->next)
		(void)orphan_signal(o->pidfd, sig);
}

/* Whether a tracked process still runs. A pidfd is readable once its process ends. */
static bool orphan_alive(const struct fyai_ctx *ctx)
{
	const struct fyai_orphan *o;
	struct pollfd pfd;

	for (o = ctx->orphans; o; o = o->next) {
		pfd.fd = o->pidfd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (o->pidfd < 0 || poll(&pfd, 1, 0) == 0)
			return true;
	}
	return false;
}

static void orphan_drop_all(struct fyai_ctx *ctx)
{
	struct fyai_orphan *o;

	while ((o = ctx->orphans)) {
		ctx->orphans = o->next;
		orphan_free(o);
	}
}

/* Add the processes below the roots, so that what a daemon started stops too. */
static void orphan_add_tree(struct fyai_ctx *ctx)
{
	struct fyai_orphan *o, *head, *scanned = NULL;
	pid_t kids[256];
	size_t n, i;
	int walked = 0;

	/* Each pass reads the processes that the one before added. */
	while (ctx->orphans != scanned && walked < ORPHAN_WALK_MAX) {
		head = ctx->orphans;
		for (o = head; o != scanned; o = o->next) {
			walked++;
			n = orphan_children_of(o->pid, kids, ARRAY_SIZE(kids));
			for (i = 0; i < n; i++)
				orphan_add(ctx, kids[i], 0);
		}
		scanned = head;
	}
}

static enum fyai_event_action orphan_kill_cb(const struct fyai_event *ev)
{
	struct fyai_ctx *ctx = ev->userdata;

	ctx->orphan_kill = NULL;
	orphan_end_all(ctx, SIGKILL);
	orphan_drop_all(ctx);
	return FYAIEA_STOP;
}

void fyai_orphan_sweep(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;
	int rc;

	if (!ctx->orphans)
		return;
	/* The tree is read before the signal: a parent that ends hides its children. */
	orphan_add_tree(ctx);
	orphan_end_all(ctx, SIGTERM);
	el = fyai_ctx_loop(ctx);
	if (el && !ctx->orphan_kill) {
		rc = fyai_event_add_timer(el, (int)ctx->cfg->sandbox_grace_ms, 0, orphan_kill_cb,
					  ctx, &ctx->orphan_kill);
		if (!rc)
			return;
	}
	/* No loop to wait on: end them now. */
	orphan_end_all(ctx, SIGKILL);
	orphan_drop_all(ctx);
}

void fyai_orphan_release(struct fyai_ctx *ctx)
{
	int left;

	orphan_stops_drop();
	if (ctx->orphan_kill) {
		fyai_event_source_remove(ctx->orphan_kill);
		ctx->orphan_kill = NULL;
	}
	if (ctx->orphans) {
		/* The invocation ends: give the processes the grace time that a sweep gives. */
		orphan_add_tree(ctx);
		orphan_end_all(ctx, SIGTERM);
		for (left = (int)ctx->cfg->sandbox_grace_ms; left > 0 && orphan_alive(ctx); left -= 20)
			(void)poll(NULL, 0, 20);
		orphan_end_all(ctx, SIGKILL);
	}
	orphan_drop_all(ctx);
}

void fyai_orphan_abandon(struct fyai_ctx *ctx)
{
	/* A forked child does not own the stops of its parent: forget them. */
	orphan_stops = NULL;
	orphan_drop_all(ctx);
	ctx->orphan_kill = NULL;
}

#endif /* FYAI_HOST_BACKEND_LINUX */
