/*
 * fyai_event.c - portable event-poll abstraction, backend-independent core
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_EVENT

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#include <libfyaml/libfyaml-util.h>

#include "fyai_event_priv.h"

/* Events dispatched per wait; a full batch simply means another pass. */
#define FYAI_EVENT_BATCH 32

fyai_event_ms_t fyai_event_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (fyai_event_ms_t)ts.tv_sec * 1000 +
	       (fyai_event_ms_t)(ts.tv_nsec / 1000000);
}

/* Recycling hides use-after-free from the sanitizers. */
#if defined(__SANITIZE_ADDRESS__)
#define FYAI_EVENT_POOL_OFF_DEFAULT 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FYAI_EVENT_POOL_OFF_DEFAULT 1
#endif
#endif
#ifndef FYAI_EVENT_POOL_OFF_DEFAULT
#define FYAI_EVENT_POOL_OFF_DEFAULT 0
#endif

static bool fyai_event_pool_disabled(const struct fyai_ctx *ctx)
{
	const char *env;

	(void)ctx;
	env = getenv("FYAI_EVENT_NO_POOL");
	if (env)
		return *env && strcmp(env, "0");
	return FYAI_EVENT_POOL_OFF_DEFAULT;
}

bool fyai_event_pool_enabled(const struct fyai_ctx *ctx)
{
	return !fyai_event_pool_disabled(ctx);
}

void fyai_event_pool_drain(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el, *el_next;
	struct fyai_event_source *src, *src_next;

	if (!ctx)
		return;

	for (el = ctx->event_loop_pool; el; el = el_next) {
		el_next = el->pool_next;
		free(el);
	}
	ctx->event_loop_pool = NULL;

	for (src = ctx->event_source_pool; src; src = src_next) {
		src_next = src->next;
		free(src);
	}
	ctx->event_source_pool = NULL;
}

struct fyai_event_loop *fyai_event_loop_create(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;

	el = fyai_event_pool_disabled(ctx) ? NULL : ctx->event_loop_pool;
	if (el) {
		ctx->event_loop_pool = el->pool_next;
		memset(el, 0, sizeof(*el));
	} else {
		el = calloc(1, sizeof(*el));
		fyai_error_check(ctx, el, err_out, "out of memory");
	}

	el->ctx = ctx;
	el->backend_fd = -1;
	/* The backend reported the reason; hand the loop back to the pool. */
	if (fyai_event_backend_create(el))
		goto err_pool;

	return el;

err_pool:
	if (fyai_event_pool_disabled(ctx)) {
		free(el);
	} else {
		el->pool_next = ctx->event_loop_pool;
		ctx->event_loop_pool = el;
	}
err_out:
	return NULL;
}

/* The one application loop, created on demand and owned by @ctx. */
struct fyai_event_loop *fyai_ctx_loop(struct fyai_ctx *ctx)
{
	if (!ctx)
		return NULL;
	if (!ctx->el)
		ctx->el = fyai_event_loop_create(ctx);
	return ctx->el;
}

/* Abandon the inherited loop in a freshly forked child. */
void fyai_ctx_loop_abandon(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;
	struct fyai_event_source *src, *next;

	if (!ctx || !ctx->el)
		return;

	el = ctx->el;
	ctx->el = NULL;

	for (src = el->sources; src; src = next) {
		next = src->next;
		/* Helper descriptors (timerfd/signalfd/pidfd) are ours; a
		 * watched fd belongs to the caller and is left alone. */
		if (src->kind != FYAIEK_FD && src->fd >= 0)
			close(src->fd);
		/*
		 * kqueue suppresses a signal's default action with SIG_IGN.
		 * Do not carry that application-loop disposition across exec.
		 * Linux only changes the mask, so restoring saved_sa there is
		 * harmless (and saved_sa is not marked valid independently).
		 */
		if (src->kind == FYAIEK_SIGNAL && src->saved_valid)
			(void)sigaction(src->signo, &src->saved_sa, NULL);
		free(src);
	}
	/* signalfd requires blocked signals. They are application-loop state,
	 * not part of the execution environment inherited by a tool child. */
	if (ctx->signal_mask_valid)
		(void)sigprocmask(SIG_SETMASK, &ctx->signal_mask, NULL);
	if (el->backend_fd >= 0)
		close(el->backend_fd);
	free(el);
}

void fyai_event_loop_destroy(struct fyai_event_loop *el)
{
	struct fyai_event_source *src, *next;

	if (!el)
		return;

	/* Disarm every surviving source before tearing the backend down. */
	for (src = el->sources; src; src = next) {
		next = src->next;
		fyai_event_backend_disarm(src);
		if (fyai_event_pool_disabled(el->ctx)) {
			free(src);
			continue;
		}
		src->next = el->ctx->event_source_pool;
		el->ctx->event_source_pool = src;
	}
	el->sources = NULL;
	el->source_count = 0;

	fyai_event_backend_destroy(el);

	if (fyai_event_pool_disabled(el->ctx)) {
		free(el);
		return;
	}
	el->pool_next = el->ctx->event_loop_pool;
	el->ctx->event_loop_pool = el;
}

unsigned int fyai_event_loop_source_count(const struct fyai_event_loop *el)
{
	return el ? el->source_count : 0;
}

void fyai_event_loop_stop(struct fyai_event_loop *el)
{
	if (el && el->depth)
		el->stop[el->depth - 1] = true;
}

void *fyai_event_source_userdata(const struct fyai_event_source *src)
{
	return src ? src->userdata : NULL;
}

void fyai_event_prepare(struct fyai_event *ev, struct fyai_event_source *src,
			unsigned int events)
{
	memset(ev, 0, sizeof(*ev));
	ev->loop = src->loop;
	ev->src = src;
	ev->userdata = src->userdata;
	ev->kind = src->kind;
	ev->events = events;
	ev->fd = src->kind == FYAIEK_FD ? src->fd : -1;
	ev->signo = src->signo;
	ev->pid = src->pid;
	ev->status = src->status;
	ev->count = 1;
}

int fyai_event_child_try_reap(struct fyai_event_source *src)
{
	pid_t rc;

	if (src->reaped)
		return 1;

	do {
		rc = waitpid(src->pid, &src->status, WNOHANG);
	} while (rc < 0 && errno == EINTR);

	if (rc == src->pid) {
		src->reaped = true;
		return 1;
	}
	if (!rc)
		return 0;
	/* ECHILD means somebody else already reaped it, or it was never ours. */
	if (errno == ECHILD) {
		src->status = 0;
		src->reaped = true;
		return 1;
	}
	return -1;
}

/* Allocate, link and arm a source. */
static int fyai_event_source_add(struct fyai_event_loop *el,
				 enum fyai_event_kind kind,
				 fyai_event_cb cb, void *userdata,
				 struct fyai_event_source **srcp)
{
	struct fyai_event_source *src;

	if (srcp)
		*srcp = NULL;

	if (!el)
		return -1;
	fyai_event_error_check(el, cb, err_out,
			 "event source needs a callback");

	src = fyai_event_pool_disabled(el->ctx) ? NULL :
		el->ctx->event_source_pool;
	if (src) {
		el->ctx->event_source_pool = src->next;
		memset(src, 0, sizeof(*src));
	} else {
		src = calloc(1, sizeof(*src));
		fyai_event_error_check(el, src, err_out, "out of memory");
	}

	src->loop = el;
	src->kind = kind;
	src->cb = cb;
	src->userdata = userdata;
	src->fd = -1;
	src->signo = -1;
	src->pid = -1;

	src->next = el->sources;
	el->sources = src;
	el->source_count++;

	if (srcp)
		*srcp = src;
	return 0;

err_out:
	return -1;
}

/* Unlink and free @src; the backend must already have been disarmed. */
static void fyai_event_source_unlink(struct fyai_event_source *src)
{
	struct fyai_event_loop *el = src->loop;
	struct fyai_event_source **pp;

	for (pp = &el->sources; *pp; pp = &(*pp)->next) {
		if (*pp == src) {
			*pp = src->next;
			el->source_count--;
			break;
		}
	}
	if (fyai_event_pool_disabled(el->ctx)) {
		free(src);
		return;
	}
	src->next = el->ctx->event_source_pool;
	el->ctx->event_source_pool = src;
}

/* Release a source that failed to arm. */
static void fyai_event_source_drop(struct fyai_event_source *src)
{
	fyai_event_backend_disarm(src);
	fyai_event_source_unlink(src);
}

/* Drop every source flagged during dispatch. */
static void fyai_event_reap_removed(struct fyai_event_loop *el)
{
	struct fyai_event_source *src, *next;

	if (!el->has_removed)
		return;

	for (src = el->sources; src; src = next) {
		next = src->next;
		if (src->removed) {
			fyai_event_backend_disarm(src);
			fyai_event_source_unlink(src);
		}
	}
	el->has_removed = false;
}

void fyai_event_source_remove(struct fyai_event_source *src)
{
	struct fyai_event_loop *el;

	if (!src || src->removed)
		return;

	el = src->loop;

	/* Withdraw from the kernel now, always. */
	fyai_event_backend_disarm(src);

	/* Removing a source from inside its own callback is legitimate and common (a
	 * pipe hits EOF and unregisters itself). */
	if (el->dispatch_depth) {
		src->removed = true;
		el->has_removed = true;
		return;
	}

	fyai_event_source_unlink(src);
}

int fyai_event_add_fd(struct fyai_event_loop *el, int fd, unsigned int events,
		      fyai_event_cb cb, void *userdata,
		      struct fyai_event_source **srcp)
{
	struct fyai_event_source *src;

	if (!el)
		return -1;
	fyai_event_error_check(el, fd >= 0, err_out, "invalid descriptor");
	fyai_event_error_check(el, events & (FYAIEV_READ | FYAIEV_WRITE), err_out,
			 "descriptor source needs READ or WRITE");

	if (fyai_event_source_add(el, FYAIEK_FD, cb, userdata, &src))
		goto err_out;

	src->fd = fd;
	src->events = events;

	/* The backend reported why; undo whatever it managed to acquire. */
	if (fyai_event_backend_arm(src)) {
		fyai_event_source_drop(src);
		goto err_out;
	}
	if (srcp)
		*srcp = src;
	return 0;

err_out:
	return -1;
}

int fyai_event_fd_modify(struct fyai_event_source *src, unsigned int events)
{
	unsigned int prev;

	if (!src || src->kind != FYAIEK_FD)
		return -1;
	fyai_event_error_check(src->loop,
			 events & (FYAIEV_READ | FYAIEV_WRITE), err_out,
			 "descriptor source needs READ or WRITE");

	prev = src->events;
	src->events = events;
	if (fyai_event_backend_arm(src)) {
		src->events = prev;
		goto err_out;
	}
	return 0;

err_out:
	return -1;
}

int fyai_event_add_timer(struct fyai_event_loop *el, fyai_event_ms_t first_ms,
			 fyai_event_ms_t interval_ms, fyai_event_cb cb,
			 void *userdata, struct fyai_event_source **srcp)
{
	struct fyai_event_source *src;

	if (fyai_event_source_add(el, FYAIEK_TIMER, cb, userdata, &src))
		return -1;

	if (first_ms < 0)
		first_ms = 0;
	src->interval_ms = interval_ms > 0 ? interval_ms : 0;
	src->oneshot = interval_ms <= 0;
	src->deadline_ms = fyai_event_now_ms() + first_ms;
	src->arm_gen++;

	if (fyai_event_backend_arm(src)) {
		fyai_event_source_drop(src);
		return -1;
	}
	if (srcp)
		*srcp = src;
	return 0;
}

int fyai_event_timer_rearm(struct fyai_event_source *src,
			   fyai_event_ms_t first_ms, fyai_event_ms_t interval_ms)
{
	if (!src || src->kind != FYAIEK_TIMER)
		return -1;

	if (first_ms < 0)
		first_ms = 0;
	src->interval_ms = interval_ms > 0 ? interval_ms : 0;
	src->oneshot = interval_ms <= 0;
	src->deadline_ms = fyai_event_now_ms() + first_ms;
	src->arm_gen++;
	return fyai_event_backend_arm(src);
}

int fyai_event_timer_disarm(struct fyai_event_source *src)
{
	if (!src || src->kind != FYAIEK_TIMER)
		return -1;

	src->deadline_ms = 0;
	return fyai_event_backend_disarm(src);
}

static enum fyai_event_action fyai_event_sleep_cb(const struct fyai_event *ev)
{
	(void)ev;
	return FYAIEA_STOP;
}

int fyai_event_sleep(struct fyai_event_loop *el, fyai_event_ms_t ms)
{
	struct fyai_event_source *src = NULL;
	int rc;

	if (!el)
		return -1;
	if (ms <= 0)
		return 0;

	/* A timer source rather than a bare run_until timeout. */
	rc = fyai_event_add_timer(el, ms, 0, fyai_event_sleep_cb, NULL, &src);
	if (rc)
		return -1;

	rc = fyai_event_loop_run_until(el, NULL, ms);
	fyai_event_source_remove(src);
	return rc;
}

int fyai_event_add_signal(struct fyai_event_loop *el, int signo,
			  fyai_event_cb cb, void *userdata,
			  struct fyai_event_source **srcp)
{
	struct fyai_event_source *src;

	if (!el)
		return -1;
	fyai_event_error_check(el, signo > 0 && signo < NSIG, err_out,
			 "invalid signal %d", signo);

	if (fyai_event_source_add(el, FYAIEK_SIGNAL, cb, userdata, &src))
		goto err_out;

	src->signo = signo;

	if (fyai_event_backend_arm(src)) {
		fyai_event_source_drop(src);
		goto err_out;
	}
	if (srcp)
		*srcp = src;
	return 0;

err_out:
	return -1;
}

int fyai_event_add_child(struct fyai_event_loop *el, pid_t pid,
			 fyai_event_cb cb, void *userdata,
			 struct fyai_event_source **srcp)
{
	struct fyai_event_source *src;

	if (!el)
		return -1;
	fyai_event_error_check(el, pid > 0, err_out,
			 "invalid pid %ld", (long)pid);

	if (fyai_event_source_add(el, FYAIEK_CHILD, cb, userdata, &src))
		goto err_out;

	src->pid = pid;
	src->oneshot = true;

	if (fyai_event_backend_arm(src)) {
		fyai_event_source_drop(src);
		goto err_out;
	}
	if (srcp)
		*srcp = src;
	return 0;

err_out:
	return -1;
}

/* Escalating child shutdown as loop state rather than a sequence of waits. */
/* Enter @t->stage, skipping any stage whose budget is zero. */
static void fyai_event_term_enter(struct fyai_event_source *child)
{
	static const int stage_sig[] = { 0, SIGTERM, SIGKILL };
	struct fyai_event_term *t = &child->term;

	while (t->stage < ARRAY_SIZE(stage_sig)) {
		if (stage_sig[t->stage])
			kill(child->pid, stage_sig[t->stage]);

		if (t->stage >= ARRAY_SIZE(t->stage_ms)) {
			fyai_event_timer_disarm(t->timer);
			return;
		}
		if (t->stage_ms[t->stage] > 0) {
			fyai_event_timer_rearm(t->timer,
					       t->stage_ms[t->stage], 0);
			return;
		}
		t->stage++;
	}
}

static enum fyai_event_action fyai_event_term_timeout(const struct fyai_event *ev)
{
	struct fyai_event_source *child = ev->userdata;

	/* This stage's budget is spent; escalate. */
	child->term.stage++;
	fyai_event_term_enter(child);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action fyai_event_term_exited(const struct fyai_event *ev)
{
	struct fyai_event_term *t = &ev->src->term;
	struct fyai_event copy = *ev;

	/* The ladder is over; retire the timer before the child source goes. */
	fyai_event_source_remove(t->timer);
	t->timer = NULL;

	if (!t->cb)
		return FYAIEA_STOP;

	copy.userdata = t->userdata;
	return t->cb(&copy);
}

int fyai_event_add_child_terminate(struct fyai_event_loop *el, pid_t pid,
				   fyai_event_ms_t grace_ms,
				   fyai_event_ms_t term_ms,
				   fyai_event_cb cb, void *userdata,
				   struct fyai_event_source **srcp)
{
	struct fyai_event_source *child, *timer;
	struct fyai_event_term *t;

	if (srcp)
		*srcp = NULL;
	if (!el || pid <= 0)
		return -1;

	if (fyai_event_add_child(el, pid, fyai_event_term_exited, NULL, &child))
		return -1;

	t = &child->term;
	t->active = true;
	t->stage_ms[0] = grace_ms > 0 ? grace_ms : 0;
	t->stage_ms[1] = term_ms > 0 ? term_ms : 0;
	t->cb = cb;
	t->userdata = userdata;

	/* The timer is created armed; disarm it immediately so entering the first
	 * stage decides the real deadline (or skips the wait entirely). */
	if (fyai_event_add_timer(el, 0, 0, fyai_event_term_timeout, child,
				 &timer)) {
		fyai_event_source_remove(child);
		return -1;
	}
	fyai_event_timer_disarm(timer);
	t->timer = timer;

	fyai_event_term_enter(child);

	if (srcp)
		*srcp = child;
	return 0;
}

struct fyai_event_child_wait {
	volatile bool exited;
	int status;
};

static enum fyai_event_action fyai_event_child_waited(const struct fyai_event *ev)
{
	struct fyai_event_child_wait *cw = ev->userdata;

	cw->exited = true;
	cw->status = ev->status;
	return FYAIEA_STOP;
}

int fyai_event_child_terminate(struct fyai_event_loop *el, pid_t pid,
			       fyai_event_ms_t grace_ms, fyai_event_ms_t term_ms,
			       int *statusp)
{
	struct fyai_event_child_wait cw;

	cw.exited = false;
	cw.status = 0;

	if (fyai_event_add_child_terminate(el, pid, grace_ms, term_ms,
					   fyai_event_child_waited, &cw, NULL))
		return -1;

	/* Unbounded: the ladder ends in SIGKILL, so the child is guaranteed to go. */
	if (fyai_event_loop_run_until(el, &cw.exited, -1) || !cw.exited)
		return -1;

	if (statusp)
		*statusp = cw.status;
	return 0;
}

/* Dispatch one batch. */
static int fyai_event_dispatch(struct fyai_event_loop *el,
			       struct fyai_event *evs, int n)
{
	enum fyai_event_action action;
	struct fyai_event_source *src;
	unsigned int arm_gen;
	int i, ran;

	el->dispatch_depth++;
	ran = 0;

	for (i = 0; i < n; i++) {
		src = evs[i].src;

		/* Removed earlier in this same pass - do not call into it. */
		if (src->removed)
			continue;

		evs[i].status = src->status;
		arm_gen = src->arm_gen;
		action = src->cb(&evs[i]);
		ran++;

		/* A one-shot source is spent once it has fired. */
		if (!src->removed && src->oneshot) {
			if (src->kind == FYAIEK_CHILD) {
				/* Withdraw now; defer only the free. */
				fyai_event_backend_disarm(src);
				src->removed = true;
				el->has_removed = true;
			} else if (src->kind == FYAIEK_TIMER &&
				   src->arm_gen == arm_gen) {
				/* Preserve a callback's rearm. */
				fyai_event_backend_disarm(src);
			}
		}

		if (action == FYAIEA_STOP) {
			el->stop[el->depth - 1] = true;
			break;
		}
		if (action == FYAIEA_ABORT) {
			el->stop[el->depth - 1] = true;
			el->abort[el->depth - 1] = true;
			break;
		}
	}

	el->dispatch_depth--;
	if (!el->dispatch_depth)
		fyai_event_reap_removed(el);
	return ran;
}

int fyai_event_loop_step(struct fyai_event_loop *el, fyai_event_ms_t timeout_ms)
{
	struct fyai_event evs[FYAI_EVENT_BATCH];
	bool own_depth;
	int n, ran;

	if (!el)
		return -1;
	fyai_event_error_check(el, el->depth < FYAI_EVENT_MAX_DEPTH, err_out,
			 "event loop nested too deeply");

	/* step() may be called directly or from within run_until(). */
	own_depth = !el->dispatch_depth;
	if (own_depth) {
		el->stop[el->depth] = false;
		el->abort[el->depth] = false;
		el->depth++;
	}

	n = fyai_event_backend_wait(el, timeout_ms, evs, FYAI_EVENT_BATCH);
	if (n < 0) {
		if (own_depth)
			el->depth--;
		return -1;
	}

	ran = n ? fyai_event_dispatch(el, evs, n) : 0;

	if (own_depth)
		el->depth--;
	return ran;

err_out:
	return -1;
}

int fyai_event_loop_run_until(struct fyai_event_loop *el,
			      const volatile bool *done,
			      fyai_event_ms_t timeout_ms)
{
	struct fyai_event evs[FYAI_EVENT_BATCH];
	fyai_event_ms_t deadline, now, wait_ms;
	unsigned int level;
	int n, rc;

	if (!el)
		return -1;
	fyai_event_error_check(el, el->depth < FYAI_EVENT_MAX_DEPTH, err_out,
			 "event loop nested too deeply");

	level = el->depth;
	el->stop[level] = false;
	el->abort[level] = false;
	el->depth++;

	deadline = timeout_ms >= 0 ? fyai_event_now_ms() + timeout_ms : -1;
	rc = 0;

	for (;;) {
		if (done && *done)
			break;
		if (el->stop[level])
			break;
		/* Nothing left to wait on. */
		if (!el->source_count)
			break;

		wait_ms = -1;
		if (deadline >= 0) {
			now = fyai_event_now_ms();
			if (now >= deadline)
				break;
			wait_ms = deadline - now;
		}

		n = fyai_event_backend_wait(el, wait_ms, evs, FYAI_EVENT_BATCH);
		if (n < 0) {
			rc = -1;
			break;
		}
		if (!n)			/* timeout or EINTR: re-check above */
			continue;

		fyai_event_dispatch(el, evs, n);
	}

	if (el->abort[level])
		rc = -1;

	el->depth--;
	return rc;

err_out:
	return -1;
}
