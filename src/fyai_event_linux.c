/*
 * fyai_event_linux.c - event-poll backend: epoll, timerfd, signalfd, pidfd
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Every source ends up as a descriptor in one epoll set: the caller's own fd
 * for FYAIEK_FD, and a timerfd, signalfd or pidfd that this backend creates
 * and owns for the other kinds. That uniformity is the whole reason the Linux
 * side is simple - the kernel already turns each facility into something
 * pollable.
 *
 * The exception is a kernel without pidfd_open (pre-5.3), where a child has
 * no pollable handle. See fyai_event_backend_wait() for the fallback.
 */

#define FYAI_MODULE FYAIEM_EVENT

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai_event_priv.h"

/* waitpid() probe interval for children without pidfds. */
#define FYAI_EVENT_CHILD_POLL_MS 20

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif

/* pidfd_open(2) wrapper. */
static int fyai_pidfd_open(pid_t pid)
{
	return (int)syscall(__NR_pidfd_open, pid, 0U);
}

static unsigned int epoll_to_fyaiev(uint32_t ep, unsigned int interest)
{
	unsigned int events = 0;

	if (ep & EPOLLIN)
		events |= FYAIEV_READ;
	if (ep & EPOLLOUT)
		events |= FYAIEV_WRITE;
	/* Report EOF alongside READ, never instead of it. */
	if (ep & (EPOLLHUP | EPOLLRDHUP))
		events |= FYAIEV_EOF | (interest & FYAIEV_READ);
	if (ep & EPOLLERR)
		events |= FYAIEV_ERROR;
	return events;
}

int fyai_event_backend_create(struct fyai_event_loop *el)
{
	el->backend_fd = epoll_create1(EPOLL_CLOEXEC);
	fyai_event_error_check(el, el->backend_fd >= 0, err_out,
			 "epoll_create1: %s", strerror(errno));
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

/* The descriptor this source is polled through, or -1 when it has none. */
static int source_pollfd(const struct fyai_event_source *src)
{
	return src->fd;
}

static int epoll_apply(struct fyai_event_source *src, int op, uint32_t mask)
{
	struct epoll_event ee;
	int rc;

	memset(&ee, 0, sizeof(ee));
	ee.events = mask;
	ee.data.ptr = src;

	rc = epoll_ctl(src->loop->backend_fd, op, source_pollfd(src), &ee);
	fyai_event_error_check(src->loop, !rc, err_out, "epoll_ctl: %s",
			       strerror(errno));
	return 0;

err_out:
	return -1;
}

static int arm_fd(struct fyai_event_source *src)
{
	uint32_t mask = EPOLLRDHUP;

	if (src->events & FYAIEV_READ)
		mask |= EPOLLIN;
	if (src->events & FYAIEV_WRITE)
		mask |= EPOLLOUT;

	return epoll_apply(src, src->armed ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
			   mask);
}

static int arm_timer(struct fyai_event_source *src)
{
	struct itimerspec its;
	fyai_event_ms_t rel;
	int rc;

	if (src->fd < 0) {
		src->fd = timerfd_create(CLOCK_MONOTONIC,
					 TFD_CLOEXEC | TFD_NONBLOCK);
		fyai_event_error_check(src->loop, src->fd >= 0, err_out,
				 "timerfd_create: %s", strerror(errno));
	}

	/* Arm relative rather than absolute. */
	rel = src->deadline_ms - fyai_event_now_ms();
	if (rel < 1)
		rel = 1;		/* 0 would disarm the timer */

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = (time_t)(rel / 1000);
	its.it_value.tv_nsec = (long)(rel % 1000) * 1000000;
	its.it_interval.tv_sec = (time_t)(src->interval_ms / 1000);
	its.it_interval.tv_nsec = (long)(src->interval_ms % 1000) * 1000000;

	rc = timerfd_settime(src->fd, 0, &its, NULL);
	fyai_event_error_check(src->loop, !rc, err_out, "timerfd_settime: %s",
			       strerror(errno));

	return epoll_apply(src, src->armed ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
			   EPOLLIN);

err_out:
	return -1;
}

static int arm_signal(struct fyai_event_source *src)
{
	sigset_t mask;
	int rc;

	if (src->armed)
		return 0;

	/* signalfd requires the signal to be blocked. */
	sigemptyset(&mask);
	sigaddset(&mask, src->signo);
	rc = sigprocmask(SIG_BLOCK, &mask, &src->saved_mask);
	fyai_event_error_check(src->loop, !rc, err_out, "sigprocmask: %s",
			       strerror(errno));
	src->saved_valid = true;

	src->fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	fyai_event_error_check(src->loop, src->fd >= 0, err_restore,
			 "signalfd: %s", strerror(errno));

	return epoll_apply(src, EPOLL_CTL_ADD, EPOLLIN);

err_restore:
	sigprocmask(SIG_SETMASK, &src->saved_mask, NULL);
	src->saved_valid = false;
err_out:
	return -1;
}

static int arm_child(struct fyai_event_source *src)
{
	if (src->armed)
		return 0;

	if (fyai_event_child_try_reap(src) == 1)
		return 0;

	src->fd = fyai_pidfd_open(src->pid);
	if (src->fd < 0) {
		/* backend_wait() polls children without pidfds. */
		return 0;
	}

	return epoll_apply(src, EPOLL_CTL_ADD, EPOLLIN);
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

	if (!rc)
		src->armed = true;
	return rc;

err_out:
	return -1;
}

int fyai_event_backend_disarm(struct fyai_event_source *src)
{
	int fd = source_pollfd(src);

	if (fd >= 0 && src->armed)
		epoll_ctl(src->loop->backend_fd, EPOLL_CTL_DEL, fd, NULL);
	src->armed = false;

	/* Descriptor sources are the caller's; every other kind polls through a
	 * descriptor this backend created, so it closes it here. */
	if (src->kind != FYAIEK_FD && src->fd >= 0) {
		close(src->fd);
		src->fd = -1;
	}

	if (src->kind == FYAIEK_SIGNAL && src->saved_valid) {
		sigprocmask(SIG_SETMASK, &src->saved_mask, NULL);
		src->saved_valid = false;
	}
	return 0;
}

/* Drain a timerfd and return the expiration count (at least 1). */
static unsigned int drain_timerfd(int fd)
{
	uint64_t ticks = 0;
	ssize_t r;

	r = read(fd, &ticks, sizeof(ticks));
	if (r != (ssize_t)sizeof(ticks) || !ticks)
		return 1;
	if (ticks > UINT_MAX)
		return UINT_MAX;
	return (unsigned int)ticks;
}

/* Drain a signalfd and return the number of deliveries read (at least 1). */
static unsigned int drain_signalfd(int fd)
{
	struct signalfd_siginfo si;
	unsigned int count = 0;
	ssize_t r;

	for (;;) {
		r = read(fd, &si, sizeof(si));
		if (r != (ssize_t)sizeof(si))
			break;
		count++;
	}
	return count ? count : 1;
}

/* Are there child sources with no pidfd? */
static bool has_unpollable_child(const struct fyai_event_loop *el)
{
	const struct fyai_event_source *src;

	for (src = el->sources; src; src = src->next) {
		if (src->kind == FYAIEK_CHILD && !src->removed &&
		    src->fd < 0 && !src->reaped)
			return true;
	}
	return false;
}

/* Collect child sources whose exit is already known - either reaped during
 * arm(), or detectable by a non-blocking probe. */
static int collect_ready_children(struct fyai_event_loop *el,
				  struct fyai_event *out, unsigned int max)
{
	struct fyai_event_source *src;
	int n = 0;

	for (src = el->sources; src && (unsigned int)n < max; src = src->next) {
		if (src->kind != FYAIEK_CHILD || src->removed)
			continue;
		if (!src->reaped && (src->fd >= 0 ||
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
	struct epoll_event evs[32];
	struct fyai_event_source *src;
	unsigned int events;
	int n, i, count, want;
	int timeout;

	/* Children already known to have exited never become epoll-readable. */
	count = collect_ready_children(el, out, max);
	if (count)
		return count;

	if (timeout_ms < 0)
		timeout = -1;
	else if (timeout_ms > INT_MAX)
		timeout = INT_MAX;
	else
		timeout = (int)timeout_ms;

	/* Bound the sleep when a child can only be observed by probing. */
	if (has_unpollable_child(el) &&
	    (timeout < 0 || timeout > FYAI_EVENT_CHILD_POLL_MS))
		timeout = FYAI_EVENT_CHILD_POLL_MS;

	want = (int)(max < 32 ? max : 32);
	n = epoll_wait(el->backend_fd, evs, want, timeout);
	if (n < 0) {
		/* EINTR is a spurious wakeup, not a failure: returning 0 lets the core re-
		 * check its done flag and deadline. */
		if (errno == EINTR)
			return 0;
		fyai_event_error_check(el, false, err_out, "epoll_wait: %s",
				 strerror(errno));
	}

	count = 0;
	for (i = 0; i < n && (unsigned int)count < max; i++) {
		src = evs[i].data.ptr;
		if (!src || src->removed)
			continue;

		switch (src->kind) {
		case FYAIEK_FD:
			events = epoll_to_fyaiev(evs[i].events, src->events);
			if (!events)
				continue;
			fyai_event_prepare(&out[count], src, events);
			break;

		case FYAIEK_TIMER:
			fyai_event_prepare(&out[count], src, FYAIEV_TIMER);
			out[count].count = drain_timerfd(src->fd);
			break;

		case FYAIEK_SIGNAL:
			fyai_event_prepare(&out[count], src, FYAIEV_SIGNAL);
			out[count].count = drain_signalfd(src->fd);
			break;

		case FYAIEK_CHILD:
			if (fyai_event_child_try_reap(src) != 1)
				continue;
			fyai_event_prepare(&out[count], src, FYAIEV_CHILD);
			out[count].status = src->status;
			break;

		default:
			continue;
		}
		count++;
	}

	/* A probe-only child may have exited while epoll_wait was blocked. */
	if ((unsigned int)count < max)
		count += collect_ready_children(el, out + count,
						max - (unsigned int)count);
	return count;

err_out:
	return -1;
}
