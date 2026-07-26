/*
 * fyai_event_priv.h - internals shared by the event core and its backends
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Not a public interface: only fyai_event.c and the per-platform backends
 * (fyai_event_linux.c, fyai_event_mac.c) include this.
 *
 * The split is deliberate. Everything that can be written once - the source
 * list, deadline arithmetic, deferred destruction, the nested-run guard -
 * lives in the core, and a backend supplies only the syscall-shaped parts
 * behind the five hooks below. Duplicating bookkeeping across two backends is
 * how portability layers drift apart.
 */

#ifndef FYAI_EVENT_PRIV_H
#define FYAI_EVENT_PRIV_H

#include <signal.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_event.h"

/* One queued deferred callback. Shared with the dump, which reports the queue
 * depth from a signal handler. */
struct fyai_defer {
	struct fyai_defer *next;
	fyai_event_defer_cb cb;
	void *userdata;
};

/* Escalating child shutdown, carried inline in the source that owns it. */
struct fyai_event_term {
	struct fyai_event_source *timer;
	fyai_event_ms_t stage_ms[2];	/* grace, post-SIGTERM */
	unsigned int stage;
	fyai_event_cb cb;
	void *userdata;
	bool active;
	bool group;			/* signal the child's process group */
};

struct fyai_event_source {
	struct fyai_event_source *next;
	struct fyai_event_loop *loop;
	enum fyai_event_kind kind;
	fyai_event_cb cb;
	void *userdata;
	unsigned int events;		/* requested interest (FD sources) */
	bool removed;			/* pending deferred destroy */
	bool oneshot;			/* disarm rather than repeat */
	bool armed;
	bool reaped;			/* CHILD: status already collected */
	int fd;				/* FD: watched fd. Linux: also the
					   timerfd/signalfd/pidfd. -1 if none */
	int signo;
	pid_t pid;
	int status;			/* CHILD: raw waitpid status */
	fyai_event_ms_t interval_ms;
	fyai_event_ms_t deadline_ms;	/* TIMER: next expiry, monotonic */
	/* Bumped on every (re)arm. */
	unsigned int arm_gen;
	/* SIGNAL: the disposition a forked child restores before exec. The
	 * process-wide mask/disposition change itself is refcounted per signal
	 * number, not held here; see fyai_event_signal_first_ref(). */
	struct sigaction saved_sa;
	bool saved_valid;

	/* CHILD: the shutdown ladder, when one was armed. */
	struct fyai_event_term term;
};

struct fyai_event_loop {
	struct fyai_event_loop *pool_next;
	struct fyai_ctx *ctx;		/* diagnostics sink; never NULL */
	struct fyai_event_source *sources;
	struct fyai_event_source *free_sources;
	unsigned int source_count;
	unsigned int depth;
	bool stop[FYAI_EVENT_MAX_DEPTH];
	bool abort[FYAI_EVENT_MAX_DEPTH];
	unsigned int dispatch_depth;
	bool has_removed;
	int backend_fd;			/* epoll fd or kqueue fd */
	void *backend;			/* backend-private state, may be NULL */

	/* Deferred work and its temporary wake source. */
	int defer_pipe[2];
	struct fyai_defer *defer_head;
	struct fyai_defer **defer_tail;
	struct fyai_event_source *defer_src;

	/* Permanent wake source for the SIGINT handler. */
	int wake_pipe[2];
	struct fyai_event_source *wake_src;
};

/* fyai_error_check() over a loop rather than a context: report and jump to the
 * cleanup label when @_cond does not hold. */
#define fyai_event_error_check(_el, _cond, _label, _fmt, ...) \
	fyai_error_check((_el)->ctx, (_cond), _label, (_fmt) , ## __VA_ARGS__)

/* Backend contract. */
int fyai_event_backend_create(struct fyai_event_loop *el);
void fyai_event_backend_destroy(struct fyai_event_loop *el);
int fyai_event_backend_arm(struct fyai_event_source *src);
int fyai_event_backend_disarm(struct fyai_event_source *src);
int fyai_event_backend_wait(struct fyai_event_loop *el,
			    fyai_event_ms_t timeout_ms,
			    struct fyai_event *out, unsigned int max);

/* Reap @src's child without blocking. */
int fyai_event_child_try_reap(struct fyai_event_source *src);

/* Track process signal state across multiple event sources. */
bool fyai_event_signal_first_ref(int signo);
bool fyai_event_signal_last_unref(int signo);

/* After fork(): the mask and dispositions are inherited, not owned here. */
void fyai_event_signal_forget(void);

/* Fill @ev from @src for a dispatch of @events. */
void fyai_event_prepare(struct fyai_event *ev, struct fyai_event_source *src,
			unsigned int events);

#endif
