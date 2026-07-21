/*
 * fyai_event.h - portable event-poll abstraction
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_EVENT_H
#define FYAI_EVENT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct fyai_ctx;
struct fyai_event_loop;
struct fyai_event_source;

enum fyai_event_kind {
	FYAIEK_FD,			/* readable/writable descriptor */
	FYAIEK_TIMER,			/* one-shot or repeating deadline */
	FYAIEK_SIGNAL,			/* a signal, delivered synchronously */
	FYAIEK_CHILD,			/* child process exit */
	FYAIEK_COUNT,
};

#define FYAIEV_READ	(1u << 0)	/* readable, or acceptable */
#define FYAIEV_WRITE	(1u << 1)	/* writable */
#define FYAIEV_EOF	(1u << 2)	/* peer hung up / read side at EOF */
#define FYAIEV_ERROR	(1u << 3)	/* descriptor error; see ev->error */
#define FYAIEV_TIMER	(1u << 4)	/* deadline reached */
#define FYAIEV_SIGNAL	(1u << 5)	/* signal delivered */
#define FYAIEV_CHILD	(1u << 6)	/* child exited; see ev->status */

enum fyai_event_action {
	FYAIEA_CONTINUE = 0,		/* keep going */
	FYAIEA_STOP,			/* unwind the innermost run, rc 0 */
	FYAIEA_ABORT,			/* unwind the innermost run, rc -1 */
};

struct fyai_event {
	struct fyai_event_loop *loop;
	struct fyai_event_source *src;
	void *userdata;
	unsigned int events;		/* FYAIEV_* bitmask */
	enum fyai_event_kind kind;
	int fd;				/* FYAIEK_FD: watched fd, else -1 */
	int error;			/* FYAIEV_ERROR: errno, else 0 */
	int signo;			/* FYAIEK_SIGNAL: the signal */
	unsigned int count;		/* coalesced count, always >= 1 */
	pid_t pid;			/* FYAIEK_CHILD: the child */
	int status;			/* FYAIEK_CHILD: raw waitpid status */
};

typedef enum fyai_event_action (*fyai_event_cb)(const struct fyai_event *ev);

typedef int64_t fyai_event_ms_t;

fyai_event_ms_t fyai_event_now_ms(void);

/* @ctx must outlive the loop. */
struct fyai_event_loop *fyai_event_loop_create(struct fyai_ctx *ctx);

/* The one application loop, created on first use and owned by @ctx. */
struct fyai_event_loop *fyai_ctx_loop(struct fyai_ctx *ctx);

/* Drop the inherited loop in a freshly forked child, so it builds its own. */
void fyai_ctx_loop_abandon(struct fyai_ctx *ctx);
void fyai_event_loop_destroy(struct fyai_event_loop *el);

void fyai_event_pool_drain(struct fyai_ctx *ctx);

bool fyai_event_pool_enabled(const struct fyai_ctx *ctx);

#define FYAI_EVENT_MAX_DEPTH 8

/* Run until *@done goes true, @timeout_ms elapses, a callback returns STOP or
 * ABORT, or no sources remain. */
int fyai_event_loop_run_until(struct fyai_event_loop *el,
			      const volatile bool *done,
			      fyai_event_ms_t timeout_ms);

/* Dispatch once, waiting at most @timeout_ms; return the event count or -1. */
int fyai_event_loop_step(struct fyai_event_loop *el, fyai_event_ms_t timeout_ms);

/* Ask the innermost active run to unwind. Safe from within a callback. */
void fyai_event_loop_stop(struct fyai_event_loop *el);

unsigned int fyai_event_loop_source_count(const struct fyai_event_loop *el);

int fyai_event_add_fd(struct fyai_event_loop *el, int fd, unsigned int events,
		      fyai_event_cb cb, void *userdata,
		      struct fyai_event_source **srcp);

int fyai_event_fd_modify(struct fyai_event_source *src, unsigned int events);

/* @interval_ms <= 0 creates a one-shot timer. */
int fyai_event_add_timer(struct fyai_event_loop *el, fyai_event_ms_t first_ms,
			 fyai_event_ms_t interval_ms, fyai_event_cb cb,
			 void *userdata, struct fyai_event_source **srcp);
int fyai_event_timer_rearm(struct fyai_event_source *src,
			   fyai_event_ms_t first_ms, fyai_event_ms_t interval_ms);
int fyai_event_timer_disarm(struct fyai_event_source *src);

/* Delay @ms while @el keeps servicing whatever else is registered on it - the
 * loop-aware replacement for sleep(3), which stalls the whole process. */
int fyai_event_sleep(struct fyai_event_loop *el, fyai_event_ms_t ms);

int fyai_event_add_signal(struct fyai_event_loop *el, int signo,
			  fyai_event_cb cb, void *userdata,
			  struct fyai_event_source **srcp);

int fyai_event_add_child(struct fyai_event_loop *el, pid_t pid,
			 fyai_event_cb cb, void *userdata,
			 struct fyai_event_source **srcp);

/* Wait @grace_ms, send SIGTERM, wait @term_ms, then send SIGKILL. */
int fyai_event_add_child_terminate(struct fyai_event_loop *el, pid_t pid,
				   fyai_event_ms_t grace_ms,
				   fyai_event_ms_t term_ms,
				   fyai_event_cb cb, void *userdata,
				   struct fyai_event_source **srcp);

/* Synchronous form of fyai_event_add_child_terminate(). */
int fyai_event_child_terminate(struct fyai_event_loop *el, pid_t pid,
			       fyai_event_ms_t grace_ms, fyai_event_ms_t term_ms,
			       int *statusp);

void fyai_event_source_remove(struct fyai_event_source *src);

void *fyai_event_source_userdata(const struct fyai_event_source *src);

#endif
