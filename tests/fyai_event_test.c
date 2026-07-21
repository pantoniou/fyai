/*
 * fyai_event_test.c - unit tests for the portable event-poll abstraction
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Every case is portable across both backends - nothing here is gated on a
 * platform, because the point of the tests is that the two backends behave
 * identically.
 *
 * Two rules keep them from going flaky:
 *
 *  - Never assert a lower bound on elapsed time. Timer cases assert firing
 *    counts and relative ordering only; a loaded or ASAN-instrumented machine
 *    makes wall-clock lower bounds meaningless.
 *  - Every wait carries a hard outer bound, so a hang fails the test instead
 *    of hanging CI.
 */

/* Diagnostics raised from this file are the test harness's own. */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_event.h"

/* A minimal context so the loop reports through the real diagnostic layer
 * rather than a special test path. */
static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx = { .cfg = &test_cfg };

/* Outer bound for any wait in this file. */
#define TEST_BOUND_MS 5000

struct counter {
	int fired;
	int order;
	unsigned int last_count;
	unsigned int last_events;
	int status;
	int signo;
	char buf[256];
	size_t len;
	int *seq;
	int seq_slot;
};

static int order_tick;

static enum fyai_event_action cb_count(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;

	c->fired++;
	c->last_count = ev->count;
	c->last_events = ev->events;
	c->order = ++order_tick;
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action cb_stop(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;

	c->fired++;
	c->last_count = ev->count;
	c->last_events = ev->events;
	return FYAIEA_STOP;
}

static enum fyai_event_action cb_abort(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;

	c->fired++;
	return FYAIEA_ABORT;
}

static void test_create_destroy(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	int p[2];

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(fyai_event_loop_source_count(el) == 0);
	fyai_event_loop_destroy(el);

	/* Destroying a loop that still owns sources is the ordinary error unwind, not
	 * a leak. */
	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!pipe(p));
	assert(!fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_count, &c, NULL));
	assert(!fyai_event_add_timer(el, 10, 0, cb_count, &c, NULL));
	assert(fyai_event_loop_source_count(el) == 2);
	fyai_event_loop_destroy(el);
	close(p[0]);
	close(p[1]);

	printf("ok - create/destroy\n");
}

static void test_pool_reuse(void)
{
	struct fyai_event_source *s1, *s2;
	struct fyai_event_loop *e1, *e2;
	struct counter c;
	bool pooled;
	int p[2];

	/* Steady state must not allocate. */
	pooled = fyai_event_pool_enabled(&test_ctx);
	memset(&c, 0, sizeof(c));
	fyai_event_pool_drain(&test_ctx);

	e1 = fyai_event_loop_create(&test_ctx);
	assert(e1);
	fyai_event_loop_destroy(e1);
	e2 = fyai_event_loop_create(&test_ctx);
	assert(e2);
	assert(!pooled || e2 == e1);

	assert(!pipe(p));
	assert(!fyai_event_add_fd(e2, p[0], FYAIEV_READ, cb_count, &c, &s1));
	fyai_event_source_remove(s1);
	assert(!fyai_event_add_fd(e2, p[0], FYAIEV_READ, cb_count, &c, &s2));
	assert(s2);
	assert(!pooled || s2 == s1);

	fyai_event_loop_destroy(e2);
	close(p[0]);
	close(p[1]);

	/* Draining an idle pool twice must be safe. */
	fyai_event_pool_drain(&test_ctx);
	fyai_event_pool_drain(&test_ctx);

	printf("ok - loops and sources are recycled\n");
}

static void test_arm_failure_is_clean(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	int p[2];

	/* A registration that fails to arm must leave nothing behind. */
	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);

	assert(!pipe(p));
	close(p[0]);
	close(p[1]);
	assert(fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_count, &c, NULL));
	assert(fyai_event_loop_source_count(el) == 0);

	/* Rejected before any source exists at all. */
	assert(fyai_event_add_signal(el, 0, cb_count, &c, NULL));
	assert(fyai_event_add_child(el, -1, cb_count, &c, NULL));
	assert(fyai_event_loop_source_count(el) == 0);

	/* A loop with nothing registered still runs and returns. */
	assert(!fyai_event_loop_run_until(el, NULL, -1));

	fyai_event_loop_destroy(el);

	/* These failures were expected; drop them so main() drains clean. */
	fyai_diag_reset(&test_cfg.diag);

	printf("ok - a failed arm leaves nothing registered\n");
}

struct reuse_case {
	struct fyai_event_loop *el;
	struct counter *second;
	int fd;
	int swapped;
};

static enum fyai_event_action cb_reuse(const struct fyai_event *ev)
{
	struct reuse_case *r = ev->userdata;

	/* Retire this source and immediately register a new one on the same
	 * descriptor, from inside a dispatch pass. */
	r->swapped++;
	fyai_event_source_remove(ev->src);
	assert(!fyai_event_add_fd(r->el, r->fd, FYAIEV_READ, cb_stop,
				  r->second, NULL));
	return FYAIEA_CONTINUE;
}

static void test_fd_reregister_in_dispatch(void)
{
	struct fyai_event_loop *el;
	struct counter first, second;
	struct reuse_case r;
	int p[2];

	/* The replacement source must keep working. */
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	memset(&r, 0, sizeof(r));

	assert(!pipe(p));
	assert(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
	assert(write(p[1], "x", 1) == 1);

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	r.el = el;
	r.fd = p[0];
	r.second = &second;

	assert(!fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_reuse, &r, NULL));

	/* The byte is never read, so a working registration keeps firing. */
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));

	assert(r.swapped == 1);
	assert(second.fired == 1);

	fyai_event_loop_destroy(el);
	close(p[0]);
	close(p[1]);
	printf("ok - a descriptor re-registered during dispatch keeps working\n");
}

static void test_idle_returns(void)
{
	struct fyai_event_loop *el;

	/* With nothing registered a run must return rather than block on an event
	 * that can never arrive. */
	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_loop_run_until(el, NULL, -1));
	fyai_event_loop_destroy(el);

	printf("ok - idle loop returns\n");
}

static void test_timer_oneshot(void)
{
	struct fyai_event_loop *el;
	struct counter c;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);

	assert(!fyai_event_add_timer(el, 5, 0, cb_stop, &c, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));
	assert(c.fired == 1);
	assert(c.last_events & FYAIEV_TIMER);
	assert(c.last_count >= 1);

	/* One-shot stays registered but disarmed, so it can be re-armed. */
	assert(fyai_event_loop_source_count(el) == 1);

	fyai_event_loop_destroy(el);
	printf("ok - one-shot timer\n");
}

static enum fyai_event_action cb_repeat(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;

	c->fired++;
	return c->fired >= 3 ? FYAIEA_STOP : FYAIEA_CONTINUE;
}

static void test_timer_repeating(void)
{
	struct fyai_event_loop *el;
	struct counter c;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);

	assert(!fyai_event_add_timer(el, 1, 1, cb_repeat, &c, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));
	assert(c.fired == 3);

	fyai_event_loop_destroy(el);
	printf("ok - repeating timer\n");
}

static enum fyai_event_action cb_rearm(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;

	c->fired++;
	if (c->fired >= 3)
		return FYAIEA_STOP;

	/* Re-arm the one-shot from inside its own callback. */
	assert(!fyai_event_timer_rearm(ev->src, 1, 0));
	return FYAIEA_CONTINUE;
}

static void test_timer_rearm_in_callback(void)
{
	struct fyai_event_loop *el;
	struct counter c;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);

	assert(!fyai_event_add_timer(el, 1, 0, cb_rearm, &c, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));
	assert(c.fired == 3);

	fyai_event_loop_destroy(el);
	printf("ok - one-shot timer re-armed from its own callback\n");
}

static void test_timer_order(void)
{
	struct fyai_event_loop *el;
	struct counter early, late;

	/* Both backends must agree that the nearer deadline comes first. */
	memset(&early, 0, sizeof(early));
	memset(&late, 0, sizeof(late));
	order_tick = 0;

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_add_timer(el, TEST_BOUND_MS, 0, cb_count, &late, NULL));
	assert(!fyai_event_add_timer(el, 1, 0, cb_count, &early, NULL));

	while (!early.fired) {
		if (fyai_event_loop_step(el, TEST_BOUND_MS) < 0)
			break;
	}

	assert(early.fired == 1);
	assert(!late.fired);

	fyai_event_loop_destroy(el);
	printf("ok - timers fire in deadline order\n");
}

static enum fyai_event_action cb_drain(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;
	ssize_t r;

	c->fired++;
	c->last_events |= ev->events;

	for (;;) {
		r = read(ev->fd, c->buf + c->len, sizeof(c->buf) - c->len - 1);
		if (r > 0) {
			c->len += (size_t)r;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		break;
	}
	c->buf[c->len] = '\0';

	/* r == 0 is real EOF; stop only once the pipe is genuinely drained. */
	if (!r) {
		fyai_event_source_remove(ev->src);
		return FYAIEA_STOP;
	}
	return FYAIEA_CONTINUE;
}

static void test_fd_read_eof(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	int p[2];

	memset(&c, 0, sizeof(c));
	assert(!pipe(p));
	assert(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);

	/* Payload written and the write end closed *before* the loop is entered. */
	assert(write(p[1], "hello", 5) == 5);
	close(p[1]);

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_drain, &c, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));

	assert(c.len == 5);
	assert(!memcmp(c.buf, "hello", 5));

	fyai_event_loop_destroy(el);
	close(p[0]);
	printf("ok - fd read with EOF and data pending\n");
}

static void test_fd_write(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	char blob[4096];
	int p[2];

	memset(&c, 0, sizeof(c));
	memset(blob, 'x', sizeof(blob));
	assert(!pipe(p));
	assert(fcntl(p[1], F_SETFL, O_NONBLOCK) == 0);
	assert(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);

	/* Fill the pipe so the write end is not immediately writable. */
	while (write(p[1], blob, sizeof(blob)) > 0)
		;
	assert(errno == EAGAIN || errno == EWOULDBLOCK);

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_add_fd(el, p[1], FYAIEV_WRITE, cb_stop, &c, NULL));

	/* Nothing drained yet: must not fire. */
	assert(fyai_event_loop_step(el, 20) == 0);
	assert(c.fired == 0);

	/* Drain the reader; now it must become writable. */
	while (read(p[0], blob, sizeof(blob)) > 0)
		;
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));
	assert(c.fired == 1);
	assert(c.last_events & FYAIEV_WRITE);

	fyai_event_loop_destroy(el);
	close(p[0]);
	close(p[1]);
	printf("ok - fd writable\n");
}

static enum fyai_event_action cb_child(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;

	c->fired++;
	c->status = ev->status;
	return FYAIEA_STOP;
}

static void test_child_exit(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	pid_t pid;

	memset(&c, 0, sizeof(c));
	pid = fork();
	assert(pid >= 0);
	if (!pid)
		_exit(42);

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_add_child(el, pid, cb_child, &c, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));

	assert(c.fired == 1);
	assert(WIFEXITED(c.status));
	assert(WEXITSTATUS(c.status) == 42);

	/* One-shot: the source is gone once it has fired. */
	assert(fyai_event_loop_source_count(el) == 0);

	fyai_event_loop_destroy(el);
	printf("ok - child exit status\n");
}

static void test_child_signalled(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	pid_t pid;

	memset(&c, 0, sizeof(c));
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		raise(SIGKILL);
		_exit(0);
	}

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_add_child(el, pid, cb_child, &c, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));

	assert(c.fired == 1);
	assert(WIFSIGNALED(c.status));
	assert(WTERMSIG(c.status) == SIGKILL);

	fyai_event_loop_destroy(el);
	printf("ok - child killed by signal\n");
}

static void test_child_already_exited(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	pid_t pid;
	int sync[2];
	char b;

	/* The case most likely to be broken and most often missed. */
	memset(&c, 0, sizeof(c));
	assert(!pipe(sync));

	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		close(sync[0]);
		/* Tell the parent we are about to exit, then do so. */
		assert(write(sync[1], "x", 1) == 1);
		close(sync[1]);
		_exit(7);
	}
	close(sync[1]);

	/* Wait for the child's write, then give it a moment to actually exit. */
	assert(read(sync[0], &b, 1) == 1);
	close(sync[0]);

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_add_child(el, pid, cb_child, &c, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));

	assert(c.fired == 1);
	assert(WIFEXITED(c.status));
	assert(WEXITSTATUS(c.status) == 7);

	fyai_event_loop_destroy(el);
	printf("ok - child already exited before registration\n");
}

static void test_child_terminate_polite(void)
{
	struct fyai_event_loop *el;
	int status = 0;
	pid_t pid;
	int sync[2];
	char b;

	/* Exits on its own during the grace stage: never signalled. */
	assert(!pipe(sync));
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		close(sync[0]);
		assert(write(sync[1], "x", 1) == 1);
		close(sync[1]);
		_exit(5);
	}
	close(sync[1]);
	assert(read(sync[0], &b, 1) == 1);
	close(sync[0]);

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_child_terminate(el, pid, TEST_BOUND_MS, 100, &status));
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 5);

	fyai_event_loop_destroy(el);
	printf("ok - child terminate, voluntary exit\n");
}

static void test_child_terminate_sigterm(void)
{
	struct fyai_event_loop *el;
	int status = 0;
	pid_t pid;

	/* Never exits on its own, but takes SIGTERM. */
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		for (;;)
			pause();
		_exit(0);
	}

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_child_terminate(el, pid, 0, TEST_BOUND_MS, &status));
	assert(WIFSIGNALED(status));
	assert(WTERMSIG(status) == SIGTERM);

	fyai_event_loop_destroy(el);
	printf("ok - child terminate, SIGTERM stage\n");
}

static void test_child_terminate_sigkill(void)
{
	struct fyai_event_loop *el;
	int status = 0;
	pid_t pid;
	int sync[2];
	char b;

	/* Ignores SIGTERM, so only SIGKILL ends it. */
	assert(!pipe(sync));
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		signal(SIGTERM, SIG_IGN);
		close(sync[0]);
		assert(write(sync[1], "x", 1) == 1);
		close(sync[1]);
		for (;;)
			pause();
		_exit(0);
	}
	close(sync[1]);
	assert(read(sync[0], &b, 1) == 1);
	close(sync[0]);

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_child_terminate(el, pid, 0, 50, &status));
	assert(WIFSIGNALED(status));
	assert(WTERMSIG(status) == SIGKILL);

	fyai_event_loop_destroy(el);
	printf("ok - child terminate, SIGKILL stage\n");
}

struct term_group {
	volatile bool done;
	unsigned int reaped;
	unsigned int want;
	int status[3];
	pid_t pid[3];
};

static enum fyai_event_action cb_term_group(const struct fyai_event *ev)
{
	struct term_group *g = ev->userdata;
	unsigned int i;

	for (i = 0; i < g->want; i++) {
		if (g->pid[i] == ev->pid)
			g->status[i] = ev->status;
	}
	if (++g->reaped == g->want)
		g->done = true;
	return FYAIEA_CONTINUE;
}

/* Fork a child: 0 exits at once, 1 waits for SIGTERM, 2 ignores SIGTERM. */
static pid_t spawn_term_victim(int kind)
{
	int sync[2];
	pid_t pid;
	char b;

	assert(!pipe(sync));
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		if (kind == 2)
			signal(SIGTERM, SIG_IGN);
		close(sync[0]);
		assert(write(sync[1], "x", 1) == 1);
		close(sync[1]);
		if (!kind)
			_exit(3);
		for (;;)
			pause();
		_exit(0);
	}
	close(sync[1]);
	/* Wait until the child has installed its disposition, or a SIGTERM
	 * sent with a zero grace budget could beat it. */
	assert(read(sync[0], &b, 1) == 1);
	close(sync[0]);
	return pid;
}

static void test_child_terminate_concurrent(void)
{
	struct fyai_event_loop *el;
	struct term_group g;
	unsigned int i;

	/* The reason the ladder is loop state and not a sequence of waits: three
	 * children, three different shutdown behaviours, one loop run. */
	static const struct {
		fyai_event_ms_t grace_ms;
		fyai_event_ms_t term_ms;
	} budget[3] = {
		{ 5000,   50 },		/* exits voluntarily within the grace */
		{    0, 5000 },		/* dies of SIGTERM within its budget */
		{    0,   60 },		/* ignores SIGTERM; only this expires */
	};

	memset(&g, 0, sizeof(g));
	g.want = 3;

	el = fyai_event_loop_create(&test_ctx);
	assert(el);

	for (i = 0; i < g.want; i++) {
		g.pid[i] = spawn_term_victim((int)i);
		assert(!fyai_event_add_child_terminate(el, g.pid[i],
						       budget[i].grace_ms,
						       budget[i].term_ms,
						       cb_term_group, &g, NULL));
	}

	assert(!fyai_event_loop_run_until(el, &g.done, TEST_BOUND_MS));
	assert(g.reaped == 3);

	/* Exited on its own before any signal landed. */
	assert(WIFEXITED(g.status[0]) && WEXITSTATUS(g.status[0]) == 3);
	/* Took SIGTERM at the first escalation. */
	assert(WIFSIGNALED(g.status[1]) && WTERMSIG(g.status[1]) == SIGTERM);
	/* Ignored it, so only SIGKILL ended it. */
	assert(WIFSIGNALED(g.status[2]) && WTERMSIG(g.status[2]) == SIGKILL);

	/* Every source retired itself: child sources and their timers. */
	assert(fyai_event_loop_source_count(el) == 0);

	fyai_event_loop_destroy(el);
	printf("ok - concurrent child termination in one loop\n");
}

static enum fyai_event_action cb_signal(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;

	c->fired++;
	c->signo = ev->signo;
	c->last_count = ev->count;
	return FYAIEA_STOP;
}

static void test_signal(void)
{
	struct fyai_event_loop *el;
	struct fyai_event_source *src;
	struct sigaction before, after;
	sigset_t mask_before, mask_after;
	struct counter c;

	memset(&c, 0, sizeof(c));
	assert(!sigaction(SIGUSR1, NULL, &before));
	assert(!sigprocmask(SIG_SETMASK, NULL, &mask_before));

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_add_signal(el, SIGUSR1, cb_signal, &c, &src));

	raise(SIGUSR1);
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));
	assert(c.fired == 1);
	assert(c.signo == SIGUSR1);
	assert(c.last_count >= 1);

	/* Removal must hand back the disposition and mask it took over. */
	fyai_event_source_remove(src);
	assert(!sigaction(SIGUSR1, NULL, &after));
	assert(!sigprocmask(SIG_SETMASK, NULL, &mask_after));
	assert(after.sa_handler == before.sa_handler);
	assert(sigismember(&mask_after, SIGUSR1) ==
	       sigismember(&mask_before, SIGUSR1));

	fyai_event_loop_destroy(el);
	printf("ok - signal delivery and disposition restore\n");
}

struct nested {
	struct fyai_event_loop *el;
	struct counter inner;
	int outer_fired;
	int inner_rc;
};

static enum fyai_event_action cb_outer(const struct fyai_event *ev)
{
	struct nested *n = ev->userdata;

	n->outer_fired++;

	/* An inner run must unwind on its own STOP, leaving the outer alone. */
	assert(!fyai_event_add_timer(n->el, 1, 0, cb_stop, &n->inner, NULL));
	n->inner_rc = fyai_event_loop_run_until(n->el, NULL, TEST_BOUND_MS);

	return FYAIEA_STOP;
}

static void test_nested_run(void)
{
	struct fyai_event_loop *el;
	struct nested n;

	memset(&n, 0, sizeof(n));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	n.el = el;

	assert(!fyai_event_add_timer(el, 1, 0, cb_outer, &n, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));

	assert(n.outer_fired == 1);
	assert(!n.inner_rc);
	assert(n.inner.fired == 1);

	fyai_event_loop_destroy(el);
	printf("ok - nested run unwinds only the inner\n");
}

struct nested_remove {
	struct fyai_event_loop *el;
	struct counter inner;
	int outer_fired;
};

static enum fyai_event_action cb_nested_remove(const struct fyai_event *ev)
{
	struct nested_remove *n = ev->userdata;

	n->outer_fired++;
	assert(!fyai_event_add_timer(n->el, 1, 0, cb_stop, &n->inner, NULL));
	assert(!fyai_event_loop_run_until(n->el, NULL, TEST_BOUND_MS));

	/* Removal must remain deferred through the outer dispatch. */
	fyai_event_source_remove(ev->src);
	return FYAIEA_STOP;
}

static void test_remove_after_nested_run(void)
{
	struct fyai_event_loop *el;
	struct nested_remove n;

	memset(&n, 0, sizeof(n));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	n.el = el;

	assert(!fyai_event_add_timer(el, 1, 0, cb_nested_remove, &n, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));
	assert(n.outer_fired == 1);
	assert(n.inner.fired == 1);
	assert(fyai_event_loop_source_count(el) == 1); /* spent inner timer */

	fyai_event_loop_destroy(el);
	printf("ok - removal after a nested run stays deferred\n");
}

static void test_abort(void)
{
	struct fyai_event_loop *el;
	struct counter c;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);

	assert(!fyai_event_add_timer(el, 1, 0, cb_abort, &c, NULL));
	assert(fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS) == -1);
	assert(c.fired == 1);

	fyai_event_loop_destroy(el);
	printf("ok - abort propagates\n");
}

static enum fyai_event_action cb_self_remove(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;

	c->fired++;
	/* Freeing here would leave the dispatch pass walking freed memory. */
	fyai_event_source_remove(ev->src);
	return FYAIEA_STOP;
}

static void test_self_remove(void)
{
	struct fyai_event_loop *el;
	struct counter c;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);

	assert(!fyai_event_add_timer(el, 1, 0, cb_self_remove, &c, NULL));
	assert(!fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS));
	assert(c.fired == 1);
	assert(fyai_event_loop_source_count(el) == 0);

	fyai_event_loop_destroy(el);
	printf("ok - remove from within own callback\n");
}

static void test_done_flag(void)
{
	struct fyai_event_loop *el;
	volatile bool done = false;
	struct counter c;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	assert(el);

	/* Already-true done flag must return before waiting on anything. */
	done = true;
	assert(!fyai_event_loop_run_until(el, &done, TEST_BOUND_MS));

	fyai_event_loop_destroy(el);
	printf("ok - done flag short-circuits\n");
}

static void test_timeout(void)
{
	struct fyai_event_loop *el;
	volatile bool done = false;
	struct counter c;
	int p[2];

	/* A timeout is a normal stop (rc 0) with the done flag still false - that is
	 * how a caller tells "finished" from "timed out". */
	memset(&c, 0, sizeof(c));
	assert(!pipe(p));

	el = fyai_event_loop_create(&test_ctx);
	assert(el);
	assert(!fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_count, &c, NULL));
	assert(!fyai_event_loop_run_until(el, &done, 20));
	assert(!done);
	assert(c.fired == 0);

	fyai_event_loop_destroy(el);
	close(p[0]);
	close(p[1]);
	printf("ok - timeout returns without completion\n");
}

/* A child that forks without exec must abandon the inherited loop. */
static enum fyai_event_action cb_count_reads(const struct fyai_event *ev)
{
	int *n = ev->userdata;
	char buf[64];

	if (ev->events & FYAIEV_READ) {
		if (read(ev->fd, buf, sizeof(buf)) > 0)
			(*n)++;
	}
	return FYAIEA_STOP;
}

static void test_fork_child_abandons_loop(void)
{
	struct fyai_event_loop *el;
	struct fyai_event_source *src = NULL;
	int p[2], q[2];
	int reads = 0;
	pid_t pid;
	int status;

	assert(!pipe(p));
	assert(!pipe(q));

	el = fyai_ctx_loop(&test_ctx);
	assert(el);
	assert(!fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_count_reads,
				  &reads, &src));

	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		struct fyai_event_loop *cel;
		struct fyai_event_source *csrc = NULL;

		/* Without this the child's registration lands in the parent's
		 * epoll set and the parent dispatches a child-address pointer. */
		fyai_ctx_loop_abandon(&test_ctx);

		cel = fyai_ctx_loop(&test_ctx);
		_exit(cel && cel != el &&
		      !fyai_event_add_fd(cel, q[0], FYAIEV_READ, cb_count_reads,
					 &reads, &csrc) ? 0 : 1);
	}

	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && !WEXITSTATUS(status));

	/* The parent's loop is intact: its own source still reports, and
	 * nothing the child registered shows up here. */
	assert(write(p[1], "x", 1) == 1);
	assert(write(q[1], "y", 1) == 1);
	assert(fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS) == 0);
	assert(reads == 1);

	fyai_event_source_remove(src);
	close(p[0]); close(p[1]);
	close(q[0]); close(q[1]);
	printf("ok - forked child abandons the inherited loop\n");
}

int main(void)
{
	assert(!fyai_diag_setup(&test_cfg.diag));

	test_create_destroy();
	test_pool_reuse();
	test_arm_failure_is_clean();
	test_fd_reregister_in_dispatch();
	test_fork_child_abandons_loop();
	test_idle_returns();
	test_done_flag();
	test_timeout();
	test_timer_oneshot();
	test_timer_repeating();
	test_timer_rearm_in_callback();
	test_timer_order();
	test_fd_read_eof();
	test_fd_write();
	test_child_exit();
	test_child_signalled();
	test_child_already_exited();
	test_child_terminate_polite();
	test_child_terminate_sigterm();
	test_child_terminate_sigkill();
	test_child_terminate_concurrent();
	test_signal();
	test_nested_run();
	test_remove_after_nested_run();
	test_abort();
	test_self_remove();

	/* Nothing above should have reported; drain and prove it. */
	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);

	/* Release the pooled loops and sources so exit is leak-clean. */
	fyai_event_pool_drain(&test_ctx);

	printf("all event tests passed\n");
	return 0;
}
