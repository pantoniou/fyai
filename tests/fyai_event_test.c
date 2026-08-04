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
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(event, create_destroy, event_create_destroy)
FYAI_TEST_ENTRY(event, pool_reuse, event_pool_reuse)
FYAI_TEST_ENTRY(event, arm_failure_is_clean, event_arm_failure_is_clean)
FYAI_TEST_ENTRY(event, fd_reregister_in_dispatch, event_fd_reregister_in_dispatch)
FYAI_TEST_ENTRY(event, fork_child_abandons_loop, event_fork_child_abandons_loop)
FYAI_TEST_ENTRY(event, idle_returns, event_idle_returns)
FYAI_TEST_ENTRY(event, done_flag, event_done_flag)
FYAI_TEST_ENTRY(event, timeout, event_timeout)
FYAI_TEST_ENTRY(event, timer_oneshot, event_timer_oneshot)
FYAI_TEST_ENTRY(event, timer_repeating, event_timer_repeating)
FYAI_TEST_ENTRY(event, timer_rearm_in_callback, event_timer_rearm_in_callback)
FYAI_TEST_ENTRY(event, timer_rearm_keeps_interval, event_timer_rearm_keeps_interval)
FYAI_TEST_ENTRY(event, timer_first_delay, event_timer_first_delay)
FYAI_TEST_ENTRY(event, timer_order, event_timer_order)
FYAI_TEST_ENTRY(event, fd_read_eof, event_fd_read_eof)
FYAI_TEST_ENTRY(event, fd_write, event_fd_write)
FYAI_TEST_ENTRY(event, child_exit, event_child_exit)
FYAI_TEST_ENTRY(event, child_signalled, event_child_signalled)
FYAI_TEST_ENTRY(event, child_already_exited, event_child_already_exited)
FYAI_TEST_ENTRY(event, child_nested_run, event_child_nested_run)
FYAI_TEST_ENTRY(event, child_terminate_polite, event_child_terminate_polite)
FYAI_TEST_ENTRY(event, child_terminate_sigterm, event_child_terminate_sigterm)
FYAI_TEST_ENTRY(event, child_terminate_sigkill, event_child_terminate_sigkill)
FYAI_TEST_ENTRY(event, child_terminate_concurrent, event_child_terminate_concurrent)
FYAI_TEST_ENTRY(event, signal, event_signal)
FYAI_TEST_ENTRY(event, signal_nested_teardown, event_signal_nested_teardown)
FYAI_TEST_ENTRY(event, interrupt_watchdog_acked, event_interrupt_watchdog_acked)
FYAI_TEST_ENTRY(event, interrupt_watchdog_escalates, event_interrupt_watchdog_escalates)
FYAI_TEST_ENTRY(event, state_dump, event_state_dump)
FYAI_TEST_ENTRY(event, nested_run, event_nested_run)
FYAI_TEST_ENTRY(event, remove_after_nested_run, event_remove_after_nested_run)
FYAI_TEST_ENTRY(event, abort, event_abort)
FYAI_TEST_ENTRY(event, stop_preserves_batch, event_stop_preserves_batch)
FYAI_TEST_ENTRY(event, self_remove, event_self_remove)
FYAI_TEST_ENTRY(event, defer_coalesce, event_defer_coalesce)
FYAI_TEST_ENTRY(event, defer_drain_once, event_defer_drain_once)
FYAI_TEST_ENTRY(event, defer_cancel, event_defer_cancel)
FYAI_TEST_ENTRY(event, defer_under_nested_run, event_defer_under_nested_run)

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
	int rc;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);
	fyai_event_loop_destroy(el);

	/* Destroying a loop that still owns sources is the ordinary error unwind, not
	 * a leak. */
	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = pipe(p);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_count, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_timer(el, 10, 0, cb_count, &c, NULL);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 2);
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
	int rc;

	/* Steady state must not allocate. */
	pooled = fyai_event_pool_enabled(&test_ctx);
	memset(&c, 0, sizeof(c));
	fyai_event_pool_drain(&test_ctx);

	e1 = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(e1);
	fyai_event_loop_destroy(e1);
	e2 = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(e2);
	FYAI_TCHECK(!pooled || e2 == e1);

	rc = pipe(p);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_fd(e2, p[0], FYAIEV_READ, cb_count, &c, &s1);
	FYAI_TCHECK(!rc);
	fyai_event_source_remove(s1);
	rc = fyai_event_add_fd(e2, p[0], FYAIEV_READ, cb_count, &c, &s2);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(s2);
	FYAI_TCHECK(!pooled || s2 == s1);

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
	int rc;

	/* A registration that fails to arm must leave nothing behind. */
	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	rc = pipe(p);
	FYAI_TCHECK(!rc);
	close(p[0]);
	close(p[1]);
	rc = fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_count, &c, NULL);
	FYAI_TCHECK(rc);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);

	/* Rejected before any source exists at all. */
	rc = fyai_event_add_signal(el, 0, cb_count, &c, NULL);
	FYAI_TCHECK(rc);
	rc = fyai_event_add_child(el, -1, cb_count, &c, NULL);
	FYAI_TCHECK(rc);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);

	/* A loop with nothing registered still runs and returns. */
	rc = fyai_event_loop_run_until(el, NULL, -1);
	FYAI_TCHECK(!rc);

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
	int rc;

	/* Retire this source and immediately register a new one on the same
	 * descriptor, from inside a dispatch pass. */
	r->swapped++;
	fyai_event_source_remove(ev->src);
	rc = fyai_event_add_fd(r->el, r->fd, FYAIEV_READ, cb_stop,
			       r->second, NULL);
	FYAI_TCHECK(!rc);
	return FYAIEA_CONTINUE;
}

static void test_fd_reregister_in_dispatch(void)
{
	struct fyai_event_loop *el;
	struct counter first, second;
	struct reuse_case r;
	int p[2];
	ssize_t nbytes;
	int rc;

	/* The replacement source must keep working. */
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	memset(&r, 0, sizeof(r));

	rc = pipe(p);
	FYAI_TCHECK(!rc);
	rc = fcntl(p[0], F_SETFL, O_NONBLOCK);
	FYAI_TCHECK(rc == 0);
	nbytes = write(p[1], "x", 1);
	FYAI_TCHECK(nbytes == 1);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	r.el = el;
	r.fd = p[0];
	r.second = &second;

	rc = fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_reuse, &r, NULL);
	FYAI_TCHECK(!rc);

	/* The byte is never read, so a working registration keeps firing. */
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	FYAI_TCHECK(r.swapped == 1);
	FYAI_TCHECK(second.fired == 1);

	fyai_event_loop_destroy(el);
	close(p[0]);
	close(p[1]);
	printf("ok - a descriptor re-registered during dispatch keeps working\n");
}

static void test_idle_returns(void)
{
	struct fyai_event_loop *el;
	int rc;

	/* With nothing registered a run must return rather than block on an event
	 * that can never arrive. */
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_loop_run_until(el, NULL, -1);
	FYAI_TCHECK(!rc);
	fyai_event_loop_destroy(el);

	printf("ok - idle loop returns\n");
}

static void test_timer_oneshot(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	int rc;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	rc = fyai_event_add_timer(el, 5, 0, cb_stop, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(c.fired == 1);
	FYAI_TCHECK(c.last_events & FYAIEV_TIMER);
	FYAI_TCHECK(c.last_count >= 1);

	/* One-shot stays registered but disarmed, so it can be re-armed. */
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 1);

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
	int rc;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	rc = fyai_event_add_timer(el, 1, 1, cb_repeat, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(c.fired == 3);

	fyai_event_loop_destroy(el);
	printf("ok - repeating timer\n");
}

static enum fyai_event_action cb_rearm(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;
	int rc;

	c->fired++;
	if (c->fired >= 3)
		return FYAIEA_STOP;

	/* Re-arm the one-shot from inside its own callback. */
	rc = fyai_event_timer_rearm(ev->src, 1, 0);
	FYAI_TCHECK(!rc);
	return FYAIEA_CONTINUE;
}

static void test_timer_rearm_in_callback(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	int rc;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	rc = fyai_event_add_timer(el, 1, 0, cb_rearm, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(c.fired == 3);

	fyai_event_loop_destroy(el);
	printf("ok - one-shot timer re-armed from its own callback\n");
}

/* Rearm with a repeat and then stop rearming: the source must keep firing. */
static enum fyai_event_action cb_rearm_heal(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;
	int rc;

	c->fired++;
	if (c->fired >= 3)
		return FYAIEA_STOP;
	if (c->fired > 1)	/* stop rearming; only the repeat can carry it */
		return FYAIEA_CONTINUE;

	rc = fyai_event_timer_rearm(ev->src, 1, 1);
	FYAI_TCHECK(!rc);
	return FYAIEA_CONTINUE;
}

/* Verify that the repeat interval remains after a callback rearms the timer. */
static void test_timer_rearm_keeps_interval(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	int rc;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	rc = fyai_event_add_timer(el, 1, 0, cb_rearm_heal, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(c.fired == 3);

	fyai_event_loop_destroy(el);
	printf("ok - a re-armed timer keeps its repeat as a safety net\n");
}

static void test_timer_order(void)
{
	struct fyai_event_loop *el;
	struct counter early, late;
	int rc;

	/* Both backends must agree that the nearer deadline comes first. */
	memset(&early, 0, sizeof(early));
	memset(&late, 0, sizeof(late));
	order_tick = 0;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_timer(el, TEST_BOUND_MS, 0, cb_count, &late, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_timer(el, 1, 0, cb_count, &early, NULL);
	FYAI_TCHECK(!rc);

	while (!early.fired) {
		if (fyai_event_loop_step(el, TEST_BOUND_MS) < 0)
			break;
	}

	FYAI_TCHECK(early.fired == 1);
	FYAI_TCHECK(!late.fired);

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
	ssize_t nbytes;
	int rc;

	memset(&c, 0, sizeof(c));
	rc = pipe(p);
	FYAI_TCHECK(!rc);
	rc = fcntl(p[0], F_SETFL, O_NONBLOCK);
	FYAI_TCHECK(rc == 0);

	/* Payload written and the write end closed *before* the loop is entered. */
	nbytes = write(p[1], "hello", 5);
	FYAI_TCHECK(nbytes == 5);
	close(p[1]);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_drain, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	FYAI_TCHECK(c.len == 5);
	FYAI_TCHECK(!memcmp(c.buf, "hello", 5));

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
	int rc;

	memset(&c, 0, sizeof(c));
	memset(blob, 'x', sizeof(blob));
	rc = pipe(p);
	FYAI_TCHECK(!rc);
	rc = fcntl(p[1], F_SETFL, O_NONBLOCK);
	FYAI_TCHECK(rc == 0);
	rc = fcntl(p[0], F_SETFL, O_NONBLOCK);
	FYAI_TCHECK(rc == 0);

	/* Fill the pipe so the write end is not immediately writable. */
	while (write(p[1], blob, sizeof(blob)) > 0)
		;
	FYAI_TCHECK(errno == EAGAIN || errno == EWOULDBLOCK);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_fd(el, p[1], FYAIEV_WRITE, cb_stop, &c, NULL);
	FYAI_TCHECK(!rc);

	/* Nothing drained yet: must not fire. */
	rc = fyai_event_loop_step(el, 20);
	FYAI_TCHECK(rc == 0);
	FYAI_TCHECK(c.fired == 0);

	/* Drain the reader; now it must become writable. */
	while (read(p[0], blob, sizeof(blob)) > 0)
		;
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(c.fired == 1);
	FYAI_TCHECK(c.last_events & FYAIEV_WRITE);

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
	int rc;

	memset(&c, 0, sizeof(c));
	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid)
		_exit(42);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_child(el, pid, cb_child, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	FYAI_TCHECK(c.fired == 1);
	FYAI_TCHECK(WIFEXITED(c.status));
	FYAI_TCHECK(WEXITSTATUS(c.status) == 42);

	/* One-shot: the source is gone once it has fired. */
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);

	fyai_event_loop_destroy(el);
	printf("ok - child exit status\n");
}

static void test_child_signalled(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	pid_t pid;
	int rc;

	memset(&c, 0, sizeof(c));
	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		raise(SIGKILL);
		_exit(0);
	}

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_child(el, pid, cb_child, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	FYAI_TCHECK(c.fired == 1);
	FYAI_TCHECK(WIFSIGNALED(c.status));
	FYAI_TCHECK(WTERMSIG(c.status) == SIGKILL);

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
	ssize_t nbytes;
	int rc;

	/* The case most likely to be broken and most often missed. */
	memset(&c, 0, sizeof(c));
	rc = pipe(sync);
	FYAI_TCHECK(!rc);

	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		close(sync[0]);
		/* Tell the parent we are about to exit, then do so. */
		nbytes = write(sync[1], "x", 1);
		FYAI_TCHECK(nbytes == 1);
		close(sync[1]);
		_exit(7);
	}
	close(sync[1]);

	/* Wait for the child's write, then give it a moment to actually exit. */
	nbytes = read(sync[0], &b, 1);
	FYAI_TCHECK(nbytes == 1);
	close(sync[0]);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_child(el, pid, cb_child, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	FYAI_TCHECK(c.fired == 1);
	FYAI_TCHECK(WIFEXITED(c.status));
	FYAI_TCHECK(WEXITSTATUS(c.status) == 7);

	fyai_event_loop_destroy(el);
	printf("ok - child already exited before registration\n");
}

struct child_nested {
	struct fyai_event_loop *el;
	struct counter inner;
	int child_fired;
	int inner_rc;
};

static enum fyai_event_action cb_child_nested(const struct fyai_event *ev)
{
	struct child_nested *n = ev->userdata;
	int rc;

	n->child_fired++;
	rc = fyai_event_add_timer(n->el, 1, 0, cb_stop, &n->inner, NULL);
	FYAI_TCHECK(!rc);
	n->inner_rc = fyai_event_loop_run_until(n->el, NULL, TEST_BOUND_MS);
	return FYAIEA_STOP;
}

static void test_child_nested_run(void)
{
	struct fyai_event_loop *el;
	struct child_nested n;
	pid_t pid;
	int rc;

	memset(&n, 0, sizeof(n));
	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid)
		_exit(0);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	n.el = el;
	rc = fyai_event_add_child(el, pid, cb_child_nested, &n, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	FYAI_TCHECK(n.child_fired == 1);
	FYAI_TCHECK(!n.inner_rc);
	FYAI_TCHECK(n.inner.fired == 1);
	/* The spent inner timer stays registered for explicit ownership cleanup. */
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 1);

	fyai_event_loop_destroy(el);
	printf("ok - child callback can run a nested loop\n");
}

static void test_child_terminate_polite(void)
{
	struct fyai_event_loop *el;
	int status = 0;
	pid_t pid;
	int sync[2];
	char b;
	ssize_t nbytes;
	int rc;

	/* Exits on its own during the grace stage: never signalled. */
	rc = pipe(sync);
	FYAI_TCHECK(!rc);
	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		close(sync[0]);
		nbytes = write(sync[1], "x", 1);
		FYAI_TCHECK(nbytes == 1);
		close(sync[1]);
		_exit(5);
	}
	close(sync[1]);
	nbytes = read(sync[0], &b, 1);
	FYAI_TCHECK(nbytes == 1);
	close(sync[0]);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_child_terminate(el, pid, TEST_BOUND_MS, 100, &status);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(WIFEXITED(status));
	FYAI_TCHECK(WEXITSTATUS(status) == 5);

	fyai_event_loop_destroy(el);
	printf("ok - child terminate, voluntary exit\n");
}

static void test_child_terminate_sigterm(void)
{
	struct fyai_event_loop *el;
	int status = 0;
	pid_t pid;
	int rc;

	/* Never exits on its own, but takes SIGTERM. */
	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		for (;;)
			pause();
		_exit(0);
	}

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_child_terminate(el, pid, 0, TEST_BOUND_MS, &status);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(WIFSIGNALED(status));
	FYAI_TCHECK(WTERMSIG(status) == SIGTERM);

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
	ssize_t nbytes;
	int rc;

	/* Ignores SIGTERM, so only SIGKILL ends it. */
	rc = pipe(sync);
	FYAI_TCHECK(!rc);
	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		signal(SIGTERM, SIG_IGN);
		close(sync[0]);
		nbytes = write(sync[1], "x", 1);
		FYAI_TCHECK(nbytes == 1);
		close(sync[1]);
		for (;;)
			pause();
		_exit(0);
	}
	close(sync[1]);
	nbytes = read(sync[0], &b, 1);
	FYAI_TCHECK(nbytes == 1);
	close(sync[0]);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_child_terminate(el, pid, 0, 50, &status);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(WIFSIGNALED(status));
	FYAI_TCHECK(WTERMSIG(status) == SIGKILL);

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
	ssize_t nbytes;
	int rc;

	rc = pipe(sync);
	FYAI_TCHECK(!rc);
	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		if (kind == 2)
			signal(SIGTERM, SIG_IGN);
		close(sync[0]);
		nbytes = write(sync[1], "x", 1);
		FYAI_TCHECK(nbytes == 1);
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
	nbytes = read(sync[0], &b, 1);
	FYAI_TCHECK(nbytes == 1);
	close(sync[0]);
	return pid;
}

static void test_child_terminate_concurrent(void)
{
	struct fyai_event_loop *el;
	struct term_group g;
	unsigned int i;
	int rc;

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
	FYAI_TCHECK(el);

	for (i = 0; i < g.want; i++) {
		g.pid[i] = spawn_term_victim((int)i);
		rc = fyai_event_add_child_terminate(el, g.pid[i],
						    budget[i].grace_ms,
						    budget[i].term_ms,
						    cb_term_group, &g, NULL);
		FYAI_TCHECK(!rc);
	}

	rc = fyai_event_loop_run_until(el, &g.done, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(g.reaped == 3);

	/* Exited on its own before any signal landed. */
	FYAI_TCHECK(WIFEXITED(g.status[0]) && WEXITSTATUS(g.status[0]) == 3);
	/* Took SIGTERM at the first escalation. */
	FYAI_TCHECK(WIFSIGNALED(g.status[1]) && WTERMSIG(g.status[1]) == SIGTERM);
	/* Ignored it, so only SIGKILL ended it. */
	FYAI_TCHECK(WIFSIGNALED(g.status[2]) && WTERMSIG(g.status[2]) == SIGKILL);

	/* Every source retired itself: child sources and their timers. */
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);

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
	int rc;

	memset(&c, 0, sizeof(c));
	rc = sigaction(SIGUSR1, NULL, &before);
	FYAI_TCHECK(!rc);
	rc = sigprocmask(SIG_SETMASK, NULL, &mask_before);
	FYAI_TCHECK(!rc);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_signal(el, SIGUSR1, cb_signal, &c, &src);
	FYAI_TCHECK(!rc);

	/* Process-directed, not raise(): macOS EVFILT_SIGNAL observes only
	 * process-directed signals, so a thread-directed raise() never trips
	 * the knote. Linux signalfd sees either, which is why this passed
	 * there and not here. */
	rc = kill(getpid(), SIGUSR1);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(c.fired == 1);
	FYAI_TCHECK(c.signo == SIGUSR1);
	FYAI_TCHECK(c.last_count >= 1);

	/* Removal must hand back the disposition and mask it took over. */
	fyai_event_source_remove(src);
	rc = sigaction(SIGUSR1, NULL, &after);
	FYAI_TCHECK(!rc);
	rc = sigprocmask(SIG_SETMASK, NULL, &mask_after);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(after.sa_handler == before.sa_handler);
	FYAI_TCHECK(sigismember(&mask_after, SIGUSR1) ==
	       sigismember(&mask_before, SIGUSR1));

	fyai_event_loop_destroy(el);
	printf("ok - signal delivery and disposition restore\n");
}

/* Remove nested signal sources in an order that is not LIFO. */
static void test_signal_nested_teardown(void)
{
	struct fyai_event_loop *el;
	struct fyai_event_source *outer = NULL, *inner = NULL;
	struct counter c1, c2;
	sigset_t mask_before, mask_after;
	int rc;

	memset(&c1, 0, sizeof(c1));
	memset(&c2, 0, sizeof(c2));
	rc = sigprocmask(SIG_SETMASK, NULL, &mask_before);
	FYAI_TCHECK(!rc);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_signal(el, SIGUSR1, cb_signal, &c1, &outer);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_signal(el, SIGUSR1, cb_signal, &c2, &inner);
	FYAI_TCHECK(!rc);

	/* Oldest first: not the LIFO order the old code assumed. */
	fyai_event_source_remove(outer);

	/* The surviving source must still see the signal. */
	rc = kill(getpid(), SIGUSR1);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(c2.fired == 1);
	FYAI_TCHECK(c2.signo == SIGUSR1);

	/* And the last one out restores the process state. */
	fyai_event_source_remove(inner);
	rc = sigprocmask(SIG_SETMASK, NULL, &mask_after);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(sigismember(&mask_after, SIGUSR1) ==
		    sigismember(&mask_before, SIGUSR1));

	fyai_event_loop_destroy(el);
	printf("ok - sources sharing a signal tear down in any order\n");
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
	int rc;

	n->outer_fired++;

	/* An inner run must unwind on its own STOP, leaving the outer alone. */
	rc = fyai_event_add_timer(n->el, 1, 0, cb_stop, &n->inner, NULL);
	FYAI_TCHECK(!rc);
	n->inner_rc = fyai_event_loop_run_until(n->el, NULL, TEST_BOUND_MS);

	return FYAIEA_STOP;
}

static void test_nested_run(void)
{
	struct fyai_event_loop *el;
	struct nested n;
	int rc;

	memset(&n, 0, sizeof(n));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	n.el = el;

	rc = fyai_event_add_timer(el, 1, 0, cb_outer, &n, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	FYAI_TCHECK(n.outer_fired == 1);
	FYAI_TCHECK(!n.inner_rc);
	FYAI_TCHECK(n.inner.fired == 1);

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
	int rc;

	n->outer_fired++;
	rc = fyai_event_add_timer(n->el, 1, 0, cb_stop, &n->inner, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(n->el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	/* Removal must remain deferred through the outer dispatch. */
	fyai_event_source_remove(ev->src);
	return FYAIEA_STOP;
}

static void test_remove_after_nested_run(void)
{
	struct fyai_event_loop *el;
	struct nested_remove n;
	int rc;

	memset(&n, 0, sizeof(n));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	n.el = el;

	rc = fyai_event_add_timer(el, 1, 0, cb_nested_remove, &n, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(n.outer_fired == 1);
	FYAI_TCHECK(n.inner.fired == 1);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 1); /* spent inner timer */

	fyai_event_loop_destroy(el);
	printf("ok - removal after a nested run stays deferred\n");
}

static void test_abort(void)
{
	struct fyai_event_loop *el;
	struct counter c;
	int rc;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	rc = fyai_event_add_timer(el, 1, 0, cb_abort, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(rc == -1);
	FYAI_TCHECK(c.fired == 1);

	fyai_event_loop_destroy(el);
	printf("ok - abort propagates\n");
}


/* Test the acknowledged and expired states of the Ctrl-C watchdog. */
#define TEST_WATCHDOG_MS 200

static void test_interrupt_watchdog_acked(void)
{
	struct fyai_event_loop *el;
	int rc;

	el = fyai_ctx_loop(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_interrupt_open(&test_ctx, TEST_WATCHDOG_MS);
	FYAI_TCHECK(!rc);

	test_ctx.interrupt_pending = 0;
	rc = kill(getpid(), SIGINT);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(test_ctx.interrupt_pending);

	/* The signal occurs before the wait. The wake pipe must end the wait. */
	rc = fyai_event_loop_step(el, TEST_BOUND_MS);
	FYAI_TCHECK(rc == 1);

	/* A live loop acknowledges well inside the budget. */
	fyai_event_interrupt_ack(&test_ctx);
	usleep(TEST_WATCHDOG_MS * 3000);
	FYAI_TCHECK(!fyai_event_interrupt_escalated());

	/* A second SIGINT must use the handler. */
	test_ctx.interrupt_pending = 0;
	rc = kill(getpid(), SIGINT);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(test_ctx.interrupt_pending);
	fyai_event_interrupt_ack(&test_ctx);

	fyai_ctx_loop_abandon(&test_ctx);
	printf("ok - an acknowledged interrupt stands the watchdog down\n");
}

static void test_interrupt_watchdog_escalates(void)
{
	int status;
	pid_t pid, w;

	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		struct fyai_ctx child_ctx = { .cfg = &test_cfg };

		if (!fyai_ctx_loop(&child_ctx) ||
		    fyai_event_interrupt_open(&child_ctx, TEST_WATCHDOG_MS))
			_exit(70);

		/* Do not acknowledge this interrupt. */
		kill(getpid(), SIGINT);
		if (!child_ctx.interrupt_pending)
			_exit(71);
		usleep(TEST_WATCHDOG_MS * 4000);
		if (!fyai_event_interrupt_escalated())
			_exit(72);

		/* The watchdog restored the default SIGINT action. */
		kill(getpid(), SIGINT);
		_exit(73);
	}

	do {
		w = waitpid(pid, &status, 0);
	} while (w < 0 && errno == EINTR);
	FYAI_TCHECK(w == pid);
	FYAI_TCHECK(WIFSIGNALED(status));
	FYAI_TCHECK(WTERMSIG(status) == SIGINT);

	printf("ok - an unacknowledged interrupt escalates to a hard kill\n");
}


struct dump_active {
	struct fyai_event_loop *el;
	int fd;
};

static enum fyai_event_action cb_dump_active(const struct fyai_event *ev)
{
	struct dump_active *active = ev->userdata;

	fyai_event_dump_to_fd(active->el, &test_ctx, active->fd);
	return FYAIEA_STOP;
}

static void dcb_dump_active(void *userdata)
{
	struct dump_active *active = userdata;

	fyai_event_dump_to_fd(active->el, &test_ctx, active->fd);
}

/* Verify that the state dump contains the event-loop state. */
static void test_state_dump(void)
{
	struct fyai_event_loop *el;
	struct dump_active active;
	struct counter c;
	char buf[4096];
	ssize_t len;
	int p[2], q[2];
	int rc;

	memset(&c, 0, sizeof(c));
	rc = pipe(p);			/* the dump target */
	FYAI_TCHECK(!rc);
	rc = pipe(q);			/* something to watch */
	FYAI_TCHECK(!rc);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_timer(el, 50, 10, cb_count, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_fd(el, q[0], FYAIEV_READ, cb_count, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_signal(el, SIGUSR1, cb_count, &c, NULL);
	FYAI_TCHECK(!rc);

	/* An earlier case left this raised; the dump reports it faithfully. */
	test_ctx.interrupt_pending = 0;

	fyai_event_dump_to_fd(el, &test_ctx, p[1]);
	len = read(p[0], buf, sizeof(buf) - 1);
	FYAI_TCHECK(len > 0);
	buf[len] = '\0';

	FYAI_TCHECK(strstr(buf, "fyai event loop dump"));
	FYAI_TCHECK(strstr(buf, "sources=3"));
	FYAI_TCHECK(strstr(buf, "dispatch_depth=0"));
	FYAI_TCHECK(strstr(buf, "callbacks=0"));
	FYAI_TCHECK(strstr(buf, "interrupt_pending=0"));
	/* One line per source, each naming its kind. */
	FYAI_TCHECK(strstr(buf, " timer "));
	FYAI_TCHECK(strstr(buf, " signal "));
	FYAI_TCHECK(strstr(buf, " fd "));
	/* The two fields that identify a stuck loop. */
	FYAI_TCHECK(strstr(buf, "in_ms="));
	FYAI_TCHECK(strstr(buf, "interval_ms=10"));
	FYAI_TCHECK(strstr(buf, "--- end ---"));
	/* A pipe is not a raw terminal: no CRs, so a redirected dump diffs. */
	FYAI_TCHECK(!strchr(buf, '\r'));

	active.el = el;
	active.fd = p[1];
	rc = fyai_event_add_timer(el, 0, 0, cb_dump_active, &active, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	len = read(p[0], buf, sizeof(buf) - 1);
	FYAI_TCHECK(len > 0);
	buf[len] = '\0';
	FYAI_TCHECK(strstr(buf, "active event elapsed_ms="));

	fyai_event_dump_to_fd(el, &test_ctx, p[1]);
	len = read(p[0], buf, sizeof(buf) - 1);
	FYAI_TCHECK(len > 0);
	buf[len] = '\0';
	FYAI_TCHECK(strstr(buf, "callbacks=1"));
	FYAI_TCHECK(strstr(buf, "longest event elapsed_ms="));

	rc = fyai_event_defer(el, dcb_dump_active, &active);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_step(el, TEST_BOUND_MS);
	FYAI_TCHECK(rc >= 0);
	len = read(p[0], buf, sizeof(buf) - 1);
	FYAI_TCHECK(len > 0);
	buf[len] = '\0';
	FYAI_TCHECK(strstr(buf, "active defer elapsed_ms="));

	fyai_event_loop_destroy(el);

	/* A missing loop must still produce a dump. */
	fyai_event_dump_to_fd(NULL, &test_ctx, p[1]);
	len = read(p[0], buf, sizeof(buf) - 1);
	FYAI_TCHECK(len > 0);
	buf[len] = '\0';
	FYAI_TCHECK(strstr(buf, "no event loop"));

	/* A closed target is dropped, not fatal. */
	close(p[0]);
	close(p[1]);
	fyai_event_dump_to_fd(NULL, &test_ctx, -1);

	close(q[0]);
	close(q[1]);
	printf("ok - the state dump reports the loop\n");
}

/* Preserve ready events when a callback stops dispatch of the current batch. */
static void test_stop_preserves_batch(void)
{
	struct fyai_event_loop *el;
	struct counter early, timer, sig;
	sigset_t block, before;
	int rc;

	memset(&early, 0, sizeof(early));
	memset(&timer, 0, sizeof(timer));
	memset(&sig, 0, sizeof(sig));

	sigemptyset(&block);
	sigaddset(&block, SIGUSR2);
	rc = sigprocmask(SIG_BLOCK, &block, &before);
	FYAI_TCHECK(!rc);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	/* All three are ready before the first wait, so they arrive in one
	 * batch; the stopper is earliest, so it is dispatched first. */
	rc = fyai_event_add_timer(el, 1, 0, cb_stop, &early, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_timer(el, 2, 0, cb_count, &timer, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_signal(el, SIGUSR2, cb_count, &sig, NULL);
	FYAI_TCHECK(!rc);

	rc = kill(getpid(), SIGUSR2);
	FYAI_TCHECK(!rc);
	usleep(20000);

	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(early.fired == 1);

	/* Give the loop every chance to redeliver what it held back. */
	rc = fyai_event_loop_run_until(el, NULL, 200);
	FYAI_TCHECK(!rc);

	FYAI_TCHECK(timer.fired == 1);
	FYAI_TCHECK(sig.fired == 1);

	fyai_event_loop_destroy(el);
	(void)sigprocmask(SIG_SETMASK, &before, NULL);
	printf("ok - a stopped batch does not drop drained events\n");
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
	int rc;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	rc = fyai_event_add_timer(el, 1, 0, cb_self_remove, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(c.fired == 1);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);

	fyai_event_loop_destroy(el);
	printf("ok - remove from within own callback\n");
}

static void test_done_flag(void)
{
	struct fyai_event_loop *el;
	volatile bool done = false;
	struct counter c;
	int rc;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	/* Already-true done flag must return before waiting on anything. */
	done = true;
	rc = fyai_event_loop_run_until(el, &done, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	fyai_event_loop_destroy(el);
	printf("ok - done flag short-circuits\n");
}

static void test_timeout(void)
{
	struct fyai_event_loop *el;
	volatile bool done = false;
	struct counter c;
	int p[2];
	int rc;

	/* A timeout is a normal stop (rc 0) with the done flag still false - that is
	 * how a caller tells "finished" from "timed out". */
	memset(&c, 0, sizeof(c));
	rc = pipe(p);
	FYAI_TCHECK(!rc);

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_count, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, &done, 20);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(!done);
	FYAI_TCHECK(c.fired == 0);

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
	struct fyai_event_source *signal_src = NULL;
	struct sigaction signal_before;
	sigset_t mask_before;
	int p[2], q[2];
	int reads = 0;
	pid_t pid;
	int status;
	ssize_t nbytes;
	pid_t w;
	int rc;

	rc = pipe(p);
	FYAI_TCHECK(!rc);
	rc = pipe(q);
	FYAI_TCHECK(!rc);
	rc = sigaction(SIGTERM, NULL, &signal_before);
	FYAI_TCHECK(!rc);
	rc = sigprocmask(SIG_SETMASK, NULL, &mask_before);
	FYAI_TCHECK(!rc);
	test_ctx.signal_mask = mask_before;
	test_ctx.signal_mask_valid = true;

	el = fyai_ctx_loop(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_fd(el, p[0], FYAIEV_READ, cb_count_reads,
			       &reads, &src);
	FYAI_TCHECK(!rc);
	rc = fyai_event_add_signal(el, SIGTERM, cb_count, NULL, &signal_src);
	FYAI_TCHECK(!rc);

	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		struct fyai_event_loop *cel;
		struct fyai_event_source *csrc = NULL;
		struct sigaction signal_after;
		sigset_t mask_after;
		bool signal_restored;

		/* Without this the child's registration lands in the parent's
		 * epoll set and the parent dispatches a child-address pointer. */
		fyai_ctx_loop_abandon(&test_ctx);
		signal_restored =
			!sigaction(SIGTERM, NULL, &signal_after) &&
			!sigprocmask(SIG_SETMASK, NULL, &mask_after) &&
			signal_after.sa_handler == signal_before.sa_handler &&
			sigismember(&mask_after, SIGTERM) ==
			sigismember(&mask_before, SIGTERM);

		cel = fyai_ctx_loop(&test_ctx);
		/* Not "cel != el": abandon() frees the loop, so an unpooled
		 * build legitimately hands the same address straight back.
		 * What must hold is that the loop is *fresh* - the parent's
		 * source did not come across with it. */
		_exit(signal_restored && cel &&
		      fyai_event_loop_source_count(cel) == 0 &&
		      !fyai_event_add_fd(cel, q[0], FYAIEV_READ, cb_count_reads,
					 &reads, &csrc) ? 0 : 1);
	}

	w = waitpid(pid, &status, 0);
	FYAI_TCHECK(w == pid);
	FYAI_TCHECK(WIFEXITED(status) && !WEXITSTATUS(status));

	/* The parent's loop is intact: its own source still reports, and
	 * nothing the child registered shows up here. */
	nbytes = write(p[1], "x", 1);
	FYAI_TCHECK(nbytes == 1);
	nbytes = write(q[1], "y", 1);
	FYAI_TCHECK(nbytes == 1);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(rc == 0);
	FYAI_TCHECK(reads == 1);

	fyai_event_source_remove(src);
	fyai_event_source_remove(signal_src);
	test_ctx.signal_mask_valid = false;
	close(p[0]); close(p[1]);
	close(q[0]); close(q[1]);
	printf("ok - forked child abandons the inherited loop\n");
}

/* Deferred "run on the next iteration" work. */

struct defer_counter {
	struct fyai_event_loop *el;
	int fired;
	int redefer_remaining;
};

static void dcb_count(void *userdata)
{
	struct defer_counter *d = userdata;

	d->fired++;
}

static void dcb_redefer(void *userdata)
{
	struct defer_counter *d = userdata;

	d->fired++;
	if (d->redefer_remaining > 0) {
		d->redefer_remaining--;
		fyai_event_defer(d->el, dcb_redefer, d);
	}
}

static void dcb_flag(void *userdata)
{
	volatile bool *done = userdata;

	*done = true;
}

static void test_defer_coalesce(void)
{
	struct fyai_event_loop *el;
	struct defer_counter a, b;
	int rc;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	a.el = b.el = el;

	/* Idle: the wakeup source does not exist yet. */
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);

	/* A repeated (cb, userdata) collapses; a distinct userdata is its own. */
	rc = fyai_event_defer(el, dcb_count, &a);
	FYAI_TCHECK(!rc);
	rc = fyai_event_defer(el, dcb_count, &a);
	FYAI_TCHECK(!rc);
	rc = fyai_event_defer(el, dcb_count, &b);
	FYAI_TCHECK(!rc);

	/* One shared source carries all pending work. */
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 1);

	rc = fyai_event_loop_step(el, TEST_BOUND_MS);
	FYAI_TCHECK(rc >= 0);

	FYAI_TCHECK(a.fired == 1);
	FYAI_TCHECK(b.fired == 1);
	/* Drained: the source withdrew itself. */
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);

	fyai_event_loop_destroy(el);
	printf("ok - deferred work coalesces and the source is lazy\n");
}

static void test_defer_drain_once(void)
{
	struct fyai_event_loop *el;
	struct defer_counter d;
	int rc;

	/* Work queued from a deferred callback waits for the next iteration, so
	 * one that re-defers itself cannot starve I/O in a single step. */
	memset(&d, 0, sizeof(d));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	d.el = el;
	d.redefer_remaining = 2;

	rc = fyai_event_defer(el, dcb_redefer, &d);
	FYAI_TCHECK(!rc);

	rc = fyai_event_loop_step(el, TEST_BOUND_MS);
	FYAI_TCHECK(rc >= 0);
	FYAI_TCHECK(d.fired == 1);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 1);

	rc = fyai_event_loop_step(el, TEST_BOUND_MS);
	FYAI_TCHECK(rc >= 0);
	FYAI_TCHECK(d.fired == 2);

	rc = fyai_event_loop_step(el, TEST_BOUND_MS);
	FYAI_TCHECK(rc >= 0);
	FYAI_TCHECK(d.fired == 3);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);

	fyai_event_loop_destroy(el);
	printf("ok - deferred work drains one generation per step\n");
}

static void test_defer_cancel(void)
{
	struct fyai_event_loop *el;
	struct defer_counter a, b;
	int rc;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	a.el = b.el = el;

	rc = fyai_event_defer(el, dcb_count, &a);
	FYAI_TCHECK(!rc);
	rc = fyai_event_defer(el, dcb_count, &b);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 1);

	/* Cancelling one leaves the other and its source. */
	fyai_event_defer_cancel(el, dcb_count, &a);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 1);
	/* Cancelling the last withdraws the source. */
	fyai_event_defer_cancel(el, dcb_count, &b);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == 0);

	/* Nothing pending: a bounded step fires neither. */
	rc = fyai_event_loop_step(el, 20);
	FYAI_TCHECK(rc >= 0);
	FYAI_TCHECK(a.fired == 0);
	FYAI_TCHECK(b.fired == 0);

	/* Cancelling absent work is a no-op. */
	fyai_event_defer_cancel(el, dcb_count, &a);

	fyai_event_loop_destroy(el);
	printf("ok - deferred work can be cancelled before it runs\n");
}

struct defer_nested {
	struct fyai_event_loop *el;
	volatile bool inner_done;
	int outer_fired;
	int inner_rc;
};

static enum fyai_event_action cb_defer_outer(const struct fyai_event *ev)
{
	struct defer_nested *n = ev->userdata;
	int rc;

	n->outer_fired++;
	/* Queue work, then block in a nested run that knows nothing of the
	 * queue. Only a source in the pollset can wake it. */
	rc = fyai_event_defer(n->el, dcb_flag, (void *)&n->inner_done);
	FYAI_TCHECK(!rc);
	n->inner_rc = fyai_event_loop_run_until(n->el, &n->inner_done,
						TEST_BOUND_MS);
	return FYAIEA_STOP;
}

static void test_defer_under_nested_run(void)
{
	struct fyai_event_loop *el;
	struct defer_nested n;
	int rc;

	memset(&n, 0, sizeof(n));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	n.el = el;

	rc = fyai_event_add_timer(el, 1, 0, cb_defer_outer, &n, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);

	FYAI_TCHECK(n.outer_fired == 1);
	FYAI_TCHECK(!n.inner_rc);
	FYAI_TCHECK(n.inner_done);

	fyai_event_loop_destroy(el);
	printf("ok - deferred work wakes a nested run\n");
}

int event_create_destroy(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_create_destroy();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_pool_reuse(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_pool_reuse();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_arm_failure_is_clean(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_arm_failure_is_clean();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_fd_reregister_in_dispatch(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_fd_reregister_in_dispatch();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_fork_child_abandons_loop(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_fork_child_abandons_loop();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_idle_returns(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_idle_returns();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_done_flag(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_done_flag();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_timeout(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_timeout();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_timer_oneshot(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_timer_oneshot();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

/*
 * A repeating timer must fire first after its *initial* delay and only then at
 * its interval. timerfd takes the two as separate values; EVFILT_TIMER has one
 * period, and the macOS back end once armed the interval and discarded the
 * initial delay. Every repeating timer then fired no sooner than a full
 * interval, whatever deadline was asked for, which stopped the progress
 * indicator from animating. Assert the contract at the portable layer, where
 * both back ends have to keep it.
 */
static enum fyai_event_action cb_first_delay(const struct fyai_event *ev)
{
	struct counter *c = ev->userdata;

	c->fired++;
	return FYAIEA_STOP;
}

static void test_timer_first_delay(void)
{
	struct fyai_event_loop *el;
	fyai_event_ms_t start, elapsed;
	struct counter c;
	int rc;

	memset(&c, 0, sizeof(c));
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	start = fyai_event_now_ms();
	rc = fyai_event_add_timer(el, 5, 2000, cb_first_delay, &c, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, NULL, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	elapsed = fyai_event_now_ms() - start;

	FYAI_TCHECK(c.fired == 1);
	FYAI_TCHECK(elapsed < 1000);

	fyai_event_loop_destroy(el);
	printf("ok - a repeating timer honours its first delay\n");
}

int event_timer_first_delay(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_timer_first_delay();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_timer_repeating(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_timer_repeating();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_timer_rearm_in_callback(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_timer_rearm_in_callback();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_timer_rearm_keeps_interval(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_timer_rearm_keeps_interval();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_timer_order(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_timer_order();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_fd_read_eof(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_fd_read_eof();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_fd_write(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_fd_write();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_child_exit(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_child_exit();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_child_signalled(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_child_signalled();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_child_already_exited(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_child_already_exited();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_child_nested_run(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_child_nested_run();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_child_terminate_polite(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_child_terminate_polite();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_child_terminate_sigterm(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_child_terminate_sigterm();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_child_terminate_sigkill(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_child_terminate_sigkill();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_child_terminate_concurrent(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_child_terminate_concurrent();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_signal(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_signal();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_signal_nested_teardown(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_signal_nested_teardown();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_interrupt_watchdog_acked(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_interrupt_watchdog_acked();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_interrupt_watchdog_escalates(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_interrupt_watchdog_escalates();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_state_dump(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_state_dump();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_nested_run(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_nested_run();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_remove_after_nested_run(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_remove_after_nested_run();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_abort(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_abort();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_stop_preserves_batch(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_stop_preserves_batch();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_self_remove(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_self_remove();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_defer_coalesce(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_defer_coalesce();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_defer_drain_once(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_defer_drain_once();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_defer_cancel(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_defer_cancel();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

int event_defer_under_nested_run(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	test_defer_under_nested_run();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);
	return 0;
}
