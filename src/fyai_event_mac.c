/*
 * fyai_event_mac.c - event-poll backend: kqueue
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * kqueue covers all four source kinds natively - EVFILT_READ/WRITE,
 * EVFILT_TIMER, EVFILT_SIGNAL and EVFILT_PROC - so unlike the Linux side
 * there are no helper descriptors to create or own. What it costs instead is
 * semantics that differ from epoll in three places, each normalized here:
 *
 *  - EVFILT_SIGNAL fires *in addition to* normal delivery and requires the
 *    signal to be unblocked, the opposite of signalfd's requirement. The
 *    disposition is set to SIG_IGN so the default action does not fire.
 *  - EVFILT_PROC reports the exit status in kev.data but does not reap; a
 *    waitpid() still has to run, exactly as on Linux.
 *  - EV_EOF arrives with kev.data holding the bytes still buffered, so EOF
 *    is reported together with READ whenever data remains.
 */

#define FYAI_MODULE FYAIEM_EVENT

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai_event_priv.h"

int fyai_event_backend_create(struct fyai_event_loop *el)
{
	el->backend_fd = kqueue();
	fyai_event_error_check(el, el->backend_fd >= 0, err_out,
			       "kqueue: %s", strerror(errno));
	return 0;

err_out:
	return -1;
}

void fyai_event_backend_destroy(struct fyai_event_loop *el)
{
	if (el->backend_fd >= 0)
		close(el->backend_fd);
	el->backend_fd = -1;
}

/* Apply one change. A quiet ENOENT/ESRCH returns 1 so an arm can distinguish
 * "nothing was registered" from success; deletes may simply ignore it. */
static int kq_change(struct fyai_event_source *src, int16_t filter,
		     uint16_t flags, uint32_t fflags, intptr_t data,
		     uintptr_t ident, bool quiet)
{
	struct kevent kev;

	EV_SET(&kev, ident, filter, flags, fflags, data, src);

	if (kevent(src->loop->backend_fd, &kev, 1, NULL, 0, NULL) < 0) {
		if (quiet && (errno == ENOENT || errno == ESRCH))
			return 1;
		fyai_event_error_check(src->loop, false, err_out,
				       "kevent: %s", strerror(errno));
	}
	return 0;

err_out:
	return -1;
}

static int arm_fd(struct fyai_event_source *src)
{
	uint16_t add = EV_ADD | EV_ENABLE;
	uint16_t del = EV_DELETE;

	/* kqueue keeps read and write interest in separate filters, so a modify is
	 * add-what-is-wanted plus delete-what-is-not. */
	if (src->events & FYAIEV_READ) {
		if (kq_change(src, EVFILT_READ, add, 0, 0,
			      (uintptr_t)src->fd, false))
			return -1;
	} else if (src->armed) {
		kq_change(src, EVFILT_READ, del, 0, 0,
			  (uintptr_t)src->fd, true);
	}

	if (src->events & FYAIEV_WRITE) {
		if (kq_change(src, EVFILT_WRITE, add, 0, 0,
			      (uintptr_t)src->fd, false))
			return -1;
	} else if (src->armed) {
		kq_change(src, EVFILT_WRITE, del, 0, 0,
			  (uintptr_t)src->fd, true);
	}
	return 0;
}

static int arm_timer(struct fyai_event_source *src)
{
	uint16_t flags = EV_ADD | EV_ENABLE;
	fyai_event_ms_t rel;

	rel = src->deadline_ms - fyai_event_now_ms();
	if (rel < 0)
		rel = 0;

	/* A repeating timer re-arms itself with its interval; a one-shot uses the
	 * remaining delay and EV_ONESHOT. */
	if (src->oneshot)
		flags |= EV_ONESHOT;
	else
		rel = src->interval_ms;

	/* EVFILT_TIMER uses milliseconds by default on macOS. */
	return kq_change(src, EVFILT_TIMER, flags, 0,
			 (intptr_t)rel, (uintptr_t)src, false);
}

static int arm_signal(struct fyai_event_source *src)
{
	struct sigaction sa;
	sigset_t mask;
	int rc;

	if (src->armed)
		return 0;

	/* EVFILT_SIGNAL observes delivery rather than replacing it, so the default
	 * action must be suppressed or a SIGINT would still kill the process. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	rc = sigaction(src->signo, &sa, &src->saved_sa);
	fyai_event_error_check(src->loop, !rc, err_out, "sigaction: %s",
			       strerror(errno));

	/* And it must not be blocked, or delivery never happens at all. */
	sigemptyset(&mask);
	sigaddset(&mask, src->signo);
	sigprocmask(SIG_UNBLOCK, &mask, &src->saved_mask);
	src->saved_valid = true;

	/* kq_change() reported why; undo what this function took over. */
	if (kq_change(src, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0,
		      (uintptr_t)src->signo, false))
		goto err_restore;

	return 0;

err_restore:
	sigaction(src->signo, &src->saved_sa, NULL);
	sigprocmask(SIG_SETMASK, &src->saved_mask, NULL);
	src->saved_valid = false;
err_out:
	return -1;
}

static int arm_child(struct fyai_event_source *src)
{
	int rc;

	if (src->armed)
		return 0;

	/* Already gone? Collect it and let the core synthesize the event. */
	if (fyai_event_child_try_reap(src) == 1)
		return 0;

	/* EV_ADD races the child: if it exits between the probe above and here,
	 * kevent fails ESRCH. */
	rc = kq_change(src, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_ONESHOT,
		       NOTE_EXIT, 0, (uintptr_t)src->pid, true);
	return rc;
}

int fyai_event_backend_arm(struct fyai_event_source *src)
{
	int rc;

	switch (src->kind) {
	case FYAIEK_FD:
		rc = arm_fd(src);
		break;
	case FYAIEK_TIMER:
		rc = arm_timer(src);
		break;
	case FYAIEK_SIGNAL:
		rc = arm_signal(src);
		break;
	case FYAIEK_CHILD:
		rc = arm_child(src);
		break;
	default:
		fyai_event_error_check(src->loop, false, err_out,
				       "bad source kind %d", (int)src->kind);
	}

	/* A positive result means a quiet registration miss. Report success to
	 * the core but leave the source unarmed so backend_wait() probes it. */
	if (!rc)
		src->armed = true;
	return rc < 0 ? rc : 0;

err_out:
	return -1;
}

int fyai_event_backend_disarm(struct fyai_event_source *src)
{
	if (!src->armed && src->kind != FYAIEK_SIGNAL)
		return 0;
	if (!src->armed)
		goto restore;

	switch (src->kind) {
	case FYAIEK_FD:
		kq_change(src, EVFILT_READ, EV_DELETE, 0, 0,
			  (uintptr_t)src->fd, true);
		kq_change(src, EVFILT_WRITE, EV_DELETE, 0, 0,
			  (uintptr_t)src->fd, true);
		break;
	case FYAIEK_TIMER:
		kq_change(src, EVFILT_TIMER, EV_DELETE, 0, 0,
			  (uintptr_t)src, true);
		break;
	case FYAIEK_SIGNAL:
		kq_change(src, EVFILT_SIGNAL, EV_DELETE, 0, 0,
			  (uintptr_t)src->signo, true);
		goto restore;
	case FYAIEK_CHILD:
		kq_change(src, EVFILT_PROC, EV_DELETE, 0, 0,
			  (uintptr_t)src->pid, true);
		break;
	default:
		break;
	}

	src->armed = false;
	return 0;

restore:
	if (src->saved_valid) {
		sigaction(src->signo, &src->saved_sa, NULL);
		sigprocmask(SIG_SETMASK, &src->saved_mask, NULL);
		src->saved_valid = false;
	}
	src->armed = false;
	return 0;
}

/* Are there child sources that kqueue will not report? */
static bool has_unpollable_child(const struct fyai_event_loop *el)
{
	const struct fyai_event_source *src;

	for (src = el->sources; src; src = src->next) {
		if (src->kind == FYAIEK_CHILD && !src->removed &&
		    !src->armed && !src->reaped)
			return true;
	}
	return false;
}

static int collect_ready_children(struct fyai_event_loop *el,
				  struct fyai_event *out, unsigned int max)
{
	struct fyai_event_source *src;
	int n = 0;

	for (src = el->sources; src && (unsigned int)n < max; src = src->next) {
		if (src->kind != FYAIEK_CHILD || src->removed)
			continue;
		if (!src->reaped && (src->armed ||
				     fyai_event_child_try_reap(src) != 1))
			continue;
		if (!src->reaped)
			continue;

		fyai_event_prepare(&out[n], src, FYAIEV_CHILD);
		out[n].status = src->status;
		n++;
	}
	return n;
}

int fyai_event_backend_wait(struct fyai_event_loop *el,
			    fyai_event_ms_t timeout_ms,
			    struct fyai_event *out, unsigned int max)
{
	struct kevent evs[32];
	struct timespec ts, *tsp;
	struct fyai_event_source *src;
	fyai_event_ms_t wait_ms;
	unsigned int events;
	int n, i, count, want;

	count = collect_ready_children(el, out, max);
	if (count)
		return count;

	wait_ms = timeout_ms;
	if (has_unpollable_child(el) && (wait_ms < 0 || wait_ms > 20))
		wait_ms = 20;

	if (wait_ms < 0) {
		tsp = NULL;
	} else {
		ts.tv_sec = (time_t)(wait_ms / 1000);
		ts.tv_nsec = (long)(wait_ms % 1000) * 1000000;
		tsp = &ts;
	}

	want = (int)(max < 32 ? max : 32);
	n = kevent(el->backend_fd, NULL, 0, evs, want, tsp);
	if (n < 0) {
		/* EINTR is a spurious wakeup; let the core re-check. */
		if (errno == EINTR)
			return 0;
		fyai_event_error_check(el, false, err_out, "kevent: %s",
				       strerror(errno));
	}

	count = 0;
	for (i = 0; i < n && (unsigned int)count < max; i++) {
		src = evs[i].udata;
		if (!src || src->removed)
			continue;

		events = 0;
		switch (evs[i].filter) {
		case EVFILT_READ:
			events = FYAIEV_READ;
			break;
		case EVFILT_WRITE:
			events = FYAIEV_WRITE;
			break;
		case EVFILT_TIMER:
			fyai_event_prepare(&out[count], src, FYAIEV_TIMER);
			out[count].count = evs[i].data > 0 ?
				(unsigned int)evs[i].data : 1;
			count++;
			continue;
		case EVFILT_SIGNAL:
			fyai_event_prepare(&out[count], src, FYAIEV_SIGNAL);
			out[count].count = evs[i].data > 0 ?
				(unsigned int)evs[i].data : 1;
			count++;
			continue;
		case EVFILT_PROC:
			if (fyai_event_child_try_reap(src) != 1)
				continue;
			fyai_event_prepare(&out[count], src, FYAIEV_CHILD);
			out[count].status = src->status;
			count++;
			continue;
		default:
			continue;
		}

		/* Read/write filters. */
		if (evs[i].flags & EV_EOF) {
			events |= FYAIEV_EOF;
			if (evs[i].filter == EVFILT_READ && evs[i].data <= 0)
				events &= ~FYAIEV_READ;
		}
		if (evs[i].flags & EV_ERROR)
			events |= FYAIEV_ERROR;

		fyai_event_prepare(&out[count], src, events);
		if (evs[i].flags & EV_ERROR)
			out[count].error = (int)evs[i].data;
		count++;
	}

	if ((unsigned int)count < max)
		count += collect_ready_children(el, out + count,
						max - (unsigned int)count);
	return count;

err_out:
	return -1;
}
