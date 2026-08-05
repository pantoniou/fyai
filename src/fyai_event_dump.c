/*
 * fyai_event_dump.c - async-signal-safe event-loop state dump
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "fyai_event_priv.h"

static volatile sig_atomic_t fyai_dump_fd = -1;
static struct fyai_event_loop *volatile fyai_dump_loop;
static struct fyai_ctx *volatile fyai_dump_ctx;

struct dump_buf {
	char b[512];
	size_t n;
	int fd;
	bool crlf;		/* a raw terminal needs CRLF */
};

static void dump_flush(struct dump_buf *d)
{
	ssize_t w;
	size_t off = 0;

	while (off < d->n) {
		w = write(d->fd, d->b + off, d->n - off);
		if (w > 0) {
			off += (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		break;			/* the target is gone; drop the rest */
	}
	d->n = 0;
}

static void dump_mem(struct dump_buf *d, const char *s, size_t len)
{
	size_t chunk;

	while (len) {
		if (d->n == sizeof(d->b))
			dump_flush(d);
		chunk = sizeof(d->b) - d->n;
		if (chunk > len)
			chunk = len;
		memcpy(d->b + d->n, s, chunk);
		d->n += chunk;
		s += chunk;
		len -= chunk;
	}
}

static void dump_nl(struct dump_buf *d)
{
	if (d->crlf)
		dump_mem(d, "\r\n", 2);
	else
		dump_mem(d, "\n", 1);
}

static void dump_str(struct dump_buf *d, const char *s)
{
	if (s)
		dump_mem(d, s, strlen(s));
	else
		dump_mem(d, "(null)", 6);
}

static void dump_num(struct dump_buf *d, long long v)
{
	char t[24];
	size_t i = sizeof(t);
	unsigned long long u;

	if (v < 0) {
		dump_mem(d, "-", 1);
		u = (unsigned long long)(-(v + 1)) + 1ULL;
	} else {
		u = (unsigned long long)v;
	}
	do {
		t[--i] = (char)('0' + (u % 10));
		u /= 10;
	} while (u);
	dump_mem(d, t + i, sizeof(t) - i);
}

static void dump_hex(struct dump_buf *d, unsigned long long v)
{
	static const char digits[] = "0123456789abcdef";
	char t[16];
	size_t i = sizeof(t);

	do {
		t[--i] = digits[v & 0xf];
		v >>= 4;
	} while (v);
	dump_mem(d, "0x", 2);
	dump_mem(d, t + i, sizeof(t) - i);
}

static void dump_ptr(struct dump_buf *d, const void *p)
{
	if (!p)
		dump_str(d, "(nil)");
	else
		dump_hex(d, (unsigned long long)(uintptr_t)p);
}

static void dump_kv(struct dump_buf *d, const char *k, long long v)
{
	dump_str(d, " ");
	dump_str(d, k);
	dump_str(d, "=");
	dump_num(d, v);
}

static const char *dump_kind_name(enum fyai_event_kind kind)
{
	switch (kind) {
	case FYAIEK_FD:		return "fd";
	case FYAIEK_TIMER:	return "timer";
	case FYAIEK_SIGNAL:	return "signal";
	case FYAIEK_CHILD:	return "child";
	default:		return "?";
	}
}

static const char *dump_track_name(enum fyai_event_track_type type)
{
	return type == FYAIET_DEFER ? "defer" : "event";
}

static void dump_track(struct dump_buf *d, const char *prefix,
		       const struct fyai_event_track *track,
		       fyai_event_ms_t elapsed_ms)
{
	dump_str(d, prefix);
	dump_str(d, dump_track_name(track->type));
	dump_kv(d, "elapsed_ms", (long long)elapsed_ms);
	dump_str(d, " src=");
	dump_ptr(d, track->src);
	if (track->type == FYAIET_EVENT) {
		dump_str(d, " kind=");
		dump_str(d, dump_kind_name(track->kind));
		dump_kv(d, "events", (long long)track->events);
		dump_kv(d, "fd", track->fd);
		if (track->kind == FYAIEK_CHILD)
			dump_kv(d, "pid", (long long)track->pid);
	}
	dump_str(d, " cb=");
	dump_hex(d, (unsigned long long)track->cb);
	dump_str(d, " ud=");
	dump_ptr(d, track->userdata);
	dump_nl(d);
}

static void dump_flags(struct dump_buf *d, const struct fyai_event_source *src)
{
	dump_str(d, " [");
	dump_str(d, src->armed ? "a" : "-");
	dump_str(d, src->removed ? "R" : "-");
	dump_str(d, src->oneshot ? "1" : "-");
	dump_str(d, src->reaped ? "x" : "-");
	dump_str(d, "]");
}

static void dump_source(struct dump_buf *d, const struct fyai_event_source *src,
			fyai_event_ms_t now)
{
	dump_str(d, "  src ");
	dump_ptr(d, src);
	dump_str(d, " ");
	dump_str(d, dump_kind_name(src->kind));
	dump_flags(d, src);
	dump_kv(d, "fd", src->fd);

	switch (src->kind) {
	case FYAIEK_FD:
		dump_kv(d, "events", (long long)src->events);
		break;
	case FYAIEK_TIMER:
		dump_kv(d, "in_ms", (long long)(src->deadline_ms - now));
		dump_kv(d, "interval_ms", (long long)src->interval_ms);
		dump_kv(d, "arm_gen", (long long)src->arm_gen);
		break;
	case FYAIEK_SIGNAL:
		dump_kv(d, "signo", src->signo);
		break;
	case FYAIEK_CHILD:
		dump_kv(d, "pid", (long long)src->pid);
		dump_kv(d, "status", src->status);
		if (src->term.active) {
			dump_kv(d, "term_stage", (long long)src->term.stage);
			dump_str(d, " term_timer=");
			dump_ptr(d, src->term.timer);
		}
		break;
	default:
		break;
	}

	dump_str(d, " cb=");
	dump_ptr(d, (const void *)(uintptr_t)src->cb);
	dump_str(d, " ud=");
	dump_ptr(d, src->userdata);
	dump_nl(d);
}

void fyai_event_dump_to_fd(struct fyai_event_loop *el, struct fyai_ctx *ctx,
			   int fd)
{
	const struct fyai_event_source *src;
	const struct fyai_defer *dfr;
	struct dump_buf d;
	struct termios tio;
	fyai_event_ms_t now;
	unsigned int i, n;

	if (fd < 0)
		return;

	d.n = 0;
	d.fd = fd;
	d.crlf = tcgetattr(fd, &tio) == 0 && !(tio.c_oflag & OPOST);
	now = fyai_event_now_ms();

	dump_nl(&d);
	dump_str(&d, "--- fyai event loop dump (pid ");
	dump_num(&d, (long long)getpid());
	dump_str(&d, ") ---");
	dump_nl(&d);

	if (ctx) {
		dump_str(&d, "ctx ");
		dump_ptr(&d, ctx);
		dump_kv(&d, "interrupt_pending",
			(long long)ctx->interrupt_pending);
		dump_kv(&d, "interrupt_seq", (long long)ctx->interrupt_seq);
		dump_kv(&d, "interrupt_seen", (long long)ctx->interrupt_seen);
		dump_kv(&d, "terminate_pending", ctx->terminate_pending);
		dump_str(&d, " watchdog=");
		dump_str(&d, fyai_event_interrupt_escalated() ?
			 "escalated" : "armed/idle");
		dump_nl(&d);
	}

	if (!el) {
		dump_str(&d, "loop (nil): no event loop on this context");
		dump_nl(&d);
		dump_str(&d, "--- end ---");
		dump_nl(&d);
		dump_flush(&d);
		return;
	}

	dump_str(&d, "loop ");
	dump_ptr(&d, el);
	dump_kv(&d, "sources", (long long)el->source_count);
	dump_kv(&d, "depth", (long long)el->depth);
	dump_kv(&d, "dispatch_depth", (long long)el->dispatch_depth);
	dump_kv(&d, "has_removed", el->has_removed);
	dump_kv(&d, "backend_fd", el->backend_fd);
	dump_kv(&d, "callbacks", (long long)el->callback_count);
	dump_nl(&d);

	for (i = 0; i < el->active_count && i < FYAI_EVENT_TRACK_MAX; i++)
		dump_track(&d, "  active ", &el->active[i],
			   now - el->active[i].started_ms);
	if (el->callback_count)
		dump_track(&d, "  longest ", &el->longest,
			   el->longest.elapsed_ms);

	n = el->depth;
	if (n > FYAI_EVENT_MAX_DEPTH)
		n = FYAI_EVENT_MAX_DEPTH;
	for (i = 0; i < n; i++) {
		dump_str(&d, "  level ");
		dump_num(&d, i);
		dump_kv(&d, "stop", el->stop[i]);
		dump_kv(&d, "abort", el->abort[i]);
		dump_nl(&d);
	}

	i = 0;
	for (dfr = el->defer_head; dfr; dfr = dfr->next)
		i++;
	dump_str(&d, "  deferred=");
	dump_num(&d, i);
	dump_str(&d, " defer_src=");
	dump_ptr(&d, el->defer_src);
	dump_str(&d, " wake_src=");
	dump_ptr(&d, el->wake_src);
	dump_nl(&d);

	i = 0;
	for (src = el->sources; src && i < 256; src = src->next, i++)
		dump_source(&d, src, now);
	if (src) {
		dump_str(&d, "  ... truncated at ");
		dump_num(&d, i);
		dump_str(&d, " sources");
		dump_nl(&d);
	}

	dump_str(&d, "--- end ---");
	dump_nl(&d);
	dump_flush(&d);
}

static void fyai_event_dump_handler(int signo)
{
	int saved_errno = errno;

	(void)signo;
	fyai_event_dump_to_fd(fyai_dump_loop, fyai_dump_ctx,
			      (int)fyai_dump_fd);
	errno = saved_errno;
}

int fyai_event_dump_open(struct fyai_ctx *ctx, int signo, int fd)
{
	struct sigaction sa;
	sigset_t unblock;
	int rc;

	if (!ctx || signo <= 0 || signo >= NSIG || fd < 0)
		return -1;

	fyai_dump_ctx = ctx;
	fyai_dump_loop = fyai_ctx_loop(ctx);
	fyai_dump_fd = fd;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fyai_event_dump_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	rc = sigaction(signo, &sa, NULL);
	if (rc)
		return -1;

	sigemptyset(&unblock);
	sigaddset(&unblock, signo);
	return sigprocmask(SIG_UNBLOCK, &unblock, NULL);
}

void fyai_event_dump_close(void)
{
	fyai_dump_loop = NULL;
	fyai_dump_ctx = NULL;
	fyai_dump_fd = -1;
}
