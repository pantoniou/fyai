/*
 * fyai_ptydrv.c - scripted PTY driver for the test suite
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The driver runs a program on a real pseudo-terminal and plays the terminal:
 * it answers the cursor, device-attribute and background queries from a screen
 * model, types the keys of a script, and waits for a state of the window
 * between two keys. It never waits for a time. The format of the script is
 * described in doc/pty-driver.md.
 *
 * Two screens are kept. The terminal sees every byte at once and answers the
 * program. The view is fed up to the end of the last complete frame, because a
 * frame paints only the cells that changed and the window is whole only there.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_cast.h"
#include "fyai_ptydrv.h"
#include "fyai_ptyscr.h"
#include "fyai_yaml_flags.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* The end of one synchronized update: the window is whole here. */
#define FRAME_END	"\x1b[?2026l"
#define FRAME_END_LEN	8

#define READY_DEFAULT	"\x1b[?25h"
#define PROMPT_MARKER	"\xE2\x9D\xAF"	/* the prompt mark of the UI */

#define DA1_QUERY	"\x1b[c"
#define DA1_REPLY	"\x1b[?62;22c"
#define CPR_QUERY	"\x1b[6n"
#define BG_QUERY_ST	"\x1b]11;?\x1b\\"
#define BG_QUERY_BEL	"\x1b]11;?\x07"

#define READ_CHUNK	65536
#define POLL_MS		100	/* the longest wait for bytes between two checks */
#define FILE_POLL_MS	20
#define REASSERT_SEC	1.0
#define TAIL_BYTES	2000
#define SESSION_DEFAULT	15.0
#define WAIT_DEFAULT	5.0
#define TERM_GRACE_SEC	2.0
/* The stack limit a program gets: fyai raises its own, and a program such as
 * valgrind starts slowly with 256 MiB of stack. */
#define STACK_LIMIT	(8UL << 20)

enum step_kind {
	STEP_TYPE,
	STEP_WRITE,
	STEP_CLICK,
	STEP_RESIZE,
	STEP_TOUCH,
	STEP_SIGNAL,
	STEP_SNAPSHOT,
	STEP_CHECKPOINT,
	STEP_MARK,
	STEP_SLEEP,
	STEP_WAIT,
	STEP_WAIT_FRAME,
	STEP_WAIT_SCREEN,
	STEP_WAIT_ROW,
	STEP_WAIT_GONE,
	STEP_WAIT_REPAINT,
	STEP_WAIT_COPY,
	STEP_FRAME,
	STEP_WAIT_COUNT,
	STEP_WAIT_FILE,
	STEP_WAIT_INPUT,
	STEP_RESUBMIT,
};

/* The modifiers a step takes besides its kind. */
#define MOD_TIMEOUT	(1u << 0)
#define MOD_COUNT	(1u << 1)
#define MOD_MORE	(1u << 2)
#define MOD_EQUALS	(1u << 3)
#define MOD_CONTAINS	(1u << 4)
#define MOD_MARKER	(1u << 5)
#define MOD_FROM	(1u << 6)
#define MOD_EXPECT	(1u << 7)
#define MOD_TRIES	(1u << 8)

struct step_def {
	const char *name;
	enum step_kind kind;
	unsigned int mods;	/* the modifiers it accepts */
	unsigned int need;	/* the modifiers it needs, one of which suffices */
};

static const struct step_def step_defs[] = {
	{ "type",		STEP_TYPE,		0, 0 },
	{ "write",		STEP_WRITE,		0, 0 },
	{ "click",		STEP_CLICK,		MOD_TIMEOUT, 0 },
	{ "resize",		STEP_RESIZE,		0, 0 },
	{ "touch",		STEP_TOUCH,		0, 0 },
	{ "signal",		STEP_SIGNAL,		0, 0 },
	{ "snapshot",		STEP_SNAPSHOT,		0, 0 },
	{ "checkpoint",		STEP_CHECKPOINT,	MOD_TIMEOUT | MOD_EXPECT, 0 },
	{ "mark",		STEP_MARK,		0, 0 },
	{ "sleep",		STEP_SLEEP,		0, 0 },
	{ "wait",		STEP_WAIT,		MOD_TIMEOUT, 0 },
	{ "wait-frame",		STEP_WAIT_FRAME,	MOD_TIMEOUT | MOD_FROM, 0 },
	{ "wait-screen",	STEP_WAIT_SCREEN,	MOD_TIMEOUT, 0 },
	{ "wait-row",		STEP_WAIT_ROW,		MOD_TIMEOUT, 0 },
	{ "wait-gone",		STEP_WAIT_GONE,		MOD_TIMEOUT, 0 },
	{ "wait-repaint",	STEP_WAIT_REPAINT,	MOD_TIMEOUT, 0 },
	{ "wait-copy",		STEP_WAIT_COPY,		MOD_TIMEOUT, 0 },
	{ "frame",		STEP_FRAME,		MOD_TIMEOUT, 0 },
	{ "wait-count",		STEP_WAIT_COUNT,	MOD_TIMEOUT | MOD_COUNT | MOD_MORE,
				MOD_COUNT | MOD_MORE },
	{ "wait-file",		STEP_WAIT_FILE,		MOD_TIMEOUT | MOD_EQUALS | MOD_CONTAINS,
				MOD_EQUALS | MOD_CONTAINS },
	{ "wait-input",		STEP_WAIT_INPUT,	MOD_TIMEOUT | MOD_MARKER, 0 },
	{ "resubmit",		STEP_RESUBMIT,		MOD_EXPECT | MOD_TRIES,
				MOD_EXPECT },
};

struct step {
	const struct step_def *def;
	size_t index;		/* position in the script, from 1 */
	const char *text;	/* the value: text, path, needle */
	size_t len;
	long n;			/* frames, signal, count, tries */
	double seconds;		/* sleep */
	int rows;		/* resize; 0 keeps the rows */
	int cols;
	const char *extra;	/* equals, contains, marker, expect */
	bool more;		/* wait-count: relative to the capture now */
	bool contains;		/* wait-file: the file holds the text */
	bool from_start;	/* wait-frame: look from the first byte */
	bool session;		/* wait until the session deadline */
	double timeout;		/* seconds, scaled; <0 is the script default */
};

struct driver {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *gb;
	double scale;
	double session_sec;
	double wait_sec;	/* default wait of a step, scaled */
	int rows, cols;
	const char *ready;
	const char *background;
	bool background_starting;
	bool emoji_narrow;
	const char *snapshot;
	regex_t *masks;		/* the text that a checkpoint hides */
	size_t nmasks;
	int exit_status;
	int exit_signal;
	bool exit_clear;
	struct step *steps;
	size_t nsteps;
	struct fyai_cast *cast;	/* the recording, or NULL */
	char **owned;		/* the expanded strings */
	size_t nowned;

	int master;
	pid_t pid;
	bool reaped;
	bool eof;
	double session_end;
	unsigned char *data;	/* what the program sent */
	size_t len;
	size_t cap;
	struct fyai_ptyscr term;
	struct fyai_ptyscr view;
	size_t view_at;
	unsigned char *held;	/* the start of a query split between reads */
	size_t nheld;
	/* The state at the last key: waits on the window count from here. */
	size_t action_start;
	size_t copies_start;
	size_t scrolled_start;
	size_t erased_start;
	size_t erases_start;
	bool resized;		/* a size asserted by the script */
	int resize_rows, resize_cols;
	bool snapshot_taken;
};

/*
 * How much longer every deadline waits. A deadline is a guess about a machine:
 * a sanitized build, or a suite run with many jobs at once, takes several
 * times as long for the same work. A value that paces a step is not scaled.
 */
static double timeout_scale(void)
{
	const char *env = getenv("FYAI_TIMEOUT_SCALE");
	double scale;
	char *end;

	if (!env || !*env)
		env = getenv("FYAI_TIMEOUT_SCALE_DEFAULT");
	if (!env || !*env)
		return 1.0;
	scale = strtod(env, &end);
	return *end || scale <= 0 ? 1.0 : scale;
}

/* Printable form of @n bytes of @p, for a message. The caller frees it. */
static char *repr_bytes(const unsigned char *p, size_t n)
{
	char *out = malloc(n * 4 + 3), *o;
	size_t i;

	if (!out)
		return NULL;
	o = out;
	*o++ = '\'';
	for (i = 0; i < n; i++) {
		if (p[i] == '\\' || p[i] == '\'')
			o += snprintf(o, 3, "\\%c", p[i]);
		else if (p[i] == '\n')
			o += snprintf(o, 3, "\\n");
		else if (p[i] == '\r')
			o += snprintf(o, 3, "\\r");
		else if (p[i] >= 0x20 && p[i] < 0x7f)
			*o++ = (char)p[i];
		else if (p[i] >= 0x80)
			*o++ = (char)p[i];
		else
			o += snprintf(o, 5, "\\x%02x", p[i]);
	}
	*o++ = '\'';
	*o = '\0';
	return out;
}

static char *tail_repr(const struct driver *d)
{
	size_t n = d->len > TAIL_BYTES ? TAIL_BYTES : d->len;

	return repr_bytes(d->data + d->len - n, n);
}

/* The rows of the view that hold text, for a message. */
static char *screen_repr(const struct driver *d)
{
	size_t size = fyai_ptyscr_row_size(&d->view);
	char *row = malloc(size);
	char *out = NULL, *grown;
	size_t used = 0, n;
	int r;

	if (!row)
		return NULL;
	out = calloc(1, 1);
	for (r = 0; out && r < d->view.rows; r++) {
		n = fyai_ptyscr_row(&d->view, r, row);
		if (!n)
			continue;
		grown = realloc(out, used + n + 4);
		if (!grown) {
			free(out);
			out = NULL;
			break;
		}
		out = grown;
		used += (size_t)snprintf(out + used, n + 4, "%s|", row);
	}
	free(row);
	return out;
}

static int append(struct driver *d, const unsigned char *p, size_t n)
{
	unsigned char *grown;
	size_t cap;

	if (d->len + n > d->cap) {
		cap = d->cap ? d->cap : 1 << 16;
		while (cap < d->len + n)
			cap *= 2;
		grown = realloc(d->data, cap);
		if (!grown)
			return -1;
		d->data = grown;
		d->cap = cap;
	}
	memcpy(d->data + d->len, p, n);
	d->len += n;
	return 0;
}

static void set_size(int fd, int rows, int cols)
{
	struct winsize ws = { .ws_row = (unsigned short)rows,
			      .ws_col = (unsigned short)cols };

	ioctl(fd, TIOCSWINSZ, &ws);
}

/* A write whose failure the next read reports: the program may be gone. */
static void write_quiet(int fd, const void *buf, size_t len)
{
	ssize_t rc = write(fd, buf, len);

	(void)rc;
}

static bool ends_with_prefix(const unsigned char *buf, size_t len,
			     const char *query, size_t k)
{
	return len >= k && !memcmp(buf + len - k, query, k);
}

/*
 * Read what the program wrote, answer its queries and add it to the capture.
 * A pseudo-terminal answers nothing, so the driver does, from the terminal
 * model fed every byte before the question: the cursor position, DA1 (every
 * terminal answers it, and the fyai probe ends with it) and, when the script
 * sets a background, OSC 11. Return the bytes read, or -1 at the end.
 */
/* The first place of @text in the @len bytes at @p, or NULL. */
static const unsigned char *find_text(const unsigned char *p, size_t len,
				      const char *text)
{
	size_t tl;

	if (!p || !text || !len)
		return NULL;
	tl = strlen(text);
	return tl ? memmem(p, len, text, tl) : NULL;
}

static ssize_t term_read(struct driver *d)
{
	static const char *const fixed[] = { CPR_QUERY, DA1_QUERY };
	const char *queries[4];
	unsigned char chunk[READ_CHUNK], *buf;
	size_t nq = 0, blen, start = 0, at, best, keep = 0, k, i, qlen;
	const char *query;
	char reply[64];
	ssize_t n;
	int rc, crow, ccol;

	n = read(d->master, chunk, sizeof(chunk));
	if (n <= 0)
		return -1;
	if (append(d, chunk, (size_t)n))
		return -1;
	fyai_cast_output(d->cast, chunk, (size_t)n);
	queries[nq++] = fixed[0];
	queries[nq++] = fixed[1];
	if (d->background) {
		queries[nq++] = BG_QUERY_ST;
		queries[nq++] = BG_QUERY_BEL;
	}
	blen = d->nheld + (size_t)n;
	buf = malloc(blen);
	if (!buf)
		return -1;
	if (d->nheld)
		memcpy(buf, d->held, d->nheld);
	memcpy(buf + d->nheld, chunk, (size_t)n);
	for (;;) {
		query = NULL;
		best = 0;
		qlen = 0;
		for (i = 0; i < nq; i++) {
			const unsigned char *hit;

			hit = find_text(buf + start, blen - start, queries[i]);
			if (!hit)
				continue;
			at = (size_t)(hit - buf);
			if (!query || at < best) {
				query = queries[i];
				best = at;
				qlen = strlen(queries[i]);
			}
		}
		if (!query)
			break;
		fyai_ptyscr_feed(&d->term, buf + start, best + qlen - start);
		if (query == fixed[0]) {
			fyai_ptyscr_cursor(&d->term, &crow, &ccol);
			rc = snprintf(reply, sizeof(reply), "\x1b[%d;%dR",
				      crow + 1, ccol + 1);
		}
		else if (query == fixed[1])
			rc = snprintf(reply, sizeof(reply), "%s", DA1_REPLY);
		else
			rc = snprintf(reply, sizeof(reply), "\x1b]11;%s\x1b\\",
				      d->background);
		if (rc > 0) {
			fyai_cast_input(d->cast, reply, (size_t)rc);
			write_quiet(d->master, reply, (size_t)rc);
		}
		start = best + qlen;
	}
	/* A query can be split between two reads: hold its start. */
	for (i = 0; i < nq; i++)
		for (k = strlen(queries[i]) - 1; k > 0; k--)
			if (ends_with_prefix(buf, blen, queries[i], k)) {
				if (k > keep)
					keep = k;
				break;
			}
	if (blen - keep < start)
		keep = blen - start;
	fyai_ptyscr_feed(&d->term, buf + start, blen - keep - start);
	free(d->held);
	d->held = NULL;
	d->nheld = 0;
	if (keep) {
		d->held = malloc(keep);
		if (d->held) {
			memcpy(d->held, buf + blen - keep, keep);
			d->nheld = keep;
		}
	}
	free(buf);
	return n;
}

/* Wait up to @ms for bytes. Return 1 for bytes, 0 for none, -1 at the end. */
static int pump(struct driver *d, int ms)
{
	struct timeval tv = { .tv_sec = ms / 1000,
			      .tv_usec = (ms % 1000) * 1000 };
	fd_set rd;
	int rc;

	if (d->eof)
		return -1;
	FD_ZERO(&rd);
	FD_SET(d->master, &rd);
	rc = select(d->master + 1, &rd, NULL, NULL, &tv);
	if (rc < 0)
		return errno == EINTR ? 0 : -1;
	if (!rc)
		return 0;
	if (term_read(d) < 0) {
		d->eof = true;
		return -1;
	}
	return 1;
}

static size_t count_of(const unsigned char *p, size_t len, const char *text)
{
	size_t tl = strlen(text), n = 0;
	const unsigned char *hit;

	if (!tl || !len || !p || !text)
		return 0;
	while ((hit = memmem(p, len, text, tl))) {
		n++;
		len -= (size_t)(hit - p) + tl;
		p = hit + tl;
	}
	return n;
}

static const unsigned char *find_from(const struct driver *d, size_t from,
				      const char *text, size_t tl)
{
	if (from >= d->len)
		return NULL;
	return memmem(d->data + from, d->len - from, text, tl);
}

/* Make the view show the window as of the last complete frame. */
static void view_sync(struct driver *d)
{
	size_t end = 0, i;

	for (i = d->len; i >= FRAME_END_LEN; i--)
		if (!memcmp(d->data + i - FRAME_END_LEN, FRAME_END,
			    FRAME_END_LEN)) {
			end = i;
			break;
		}
	if (end > d->view_at) {
		fyai_ptyscr_feed(&d->view, d->data + d->view_at,
				 end - d->view_at);
		d->view_at = end;
	}
}

static void mark(struct driver *d)
{
	d->action_start = d->len;
	d->copies_start = d->term.clipboard.n;
	view_sync(d);
	d->scrolled_start = d->view.scrollback.n;
	d->erased_start = d->view.erased.n;
	d->erases_start = d->view.display_erases;
}

struct step_wait {
	const struct step *st;
	char *expect;		/* checkpoint: the screen to reach */
	size_t expect_len;
	long target;		/* wait-count: the count to reach */
	size_t checked;		/* wait-input: the capture last checked */
	bool held;		/* and what it said */
};

typedef bool (*pred_fn)(struct driver *d, struct step_wait *w);

/*
 * Read until @pred holds or @deadline passes. A size the script asked for is
 * stated again each second: one SIGWINCH lost on a slow runner must not fail
 * a case.
 */
static bool wait_until(struct driver *d, pred_fn pred, struct step_wait *w,
		       double deadline)
{
	double reassert = fyai_monotonic_sec(), t, left;
	int ms;

	while (!pred(d, w)) {
		t = fyai_monotonic_sec();
		if (t >= deadline)
			break;
		if (d->resized && t - reassert >= REASSERT_SEC) {
			set_size(d->master, d->resize_rows, d->resize_cols);
			reassert = t;
		}
		left = (deadline - t) * 1000.0;
		ms = left < POLL_MS ? (int)left + 1 : POLL_MS;
		if (pump(d, ms) < 0)
			break;
	}
	return pred(d, w);
}

static bool p_contains(struct driver *d, struct step_wait *w)
{
	return find_from(d, 0, w->st->text, w->st->len) != NULL;
}

static bool p_frame_text(struct driver *d, struct step_wait *w)
{
	const unsigned char *hit;

	hit = find_from(d, w->st->from_start ? 0 : d->action_start,
			w->st->text, w->st->len);
	return hit && find_from(d, (size_t)(hit - d->data), FRAME_END,
				FRAME_END_LEN);
}

static bool p_count(struct driver *d, struct step_wait *w)
{
	return (long)count_of(d->data, d->len, w->st->text) >= w->target;
}

static bool p_frames(struct driver *d, struct step_wait *w)
{
	size_t n = 0, from = d->action_start;
	const unsigned char *hit;

	while (from <= d->len &&
	       (hit = find_from(d, from, FRAME_END, FRAME_END_LEN))) {
		n++;
		from = (size_t)(hit - d->data) + FRAME_END_LEN;
	}
	return (long)n >= w->st->n;
}

static bool p_screen(struct driver *d, struct step_wait *w)
{
	view_sync(d);
	return fyai_ptyscr_has(&d->view, w->st->text, d->scrolled_start,
			       d->erased_start, false);
}

static bool p_row(struct driver *d, struct step_wait *w)
{
	view_sync(d);
	return fyai_ptyscr_has(&d->view, w->st->text, d->scrolled_start,
			       d->erased_start, true);
}

static bool p_gone(struct driver *d, struct step_wait *w)
{
	view_sync(d);
	return !fyai_ptyscr_has(&d->view, w->st->text, SIZE_MAX, SIZE_MAX,
				false);
}

/* An erase clears every row, so the text was painted again after it. */
static bool p_repaint(struct driver *d, struct step_wait *w)
{
	view_sync(d);
	return d->view.display_erases > d->erases_start &&
	       fyai_ptyscr_has(&d->view, w->st->text, SIZE_MAX, SIZE_MAX,
			       false);
}

/* A copy is terminal state, not a cell of the screen. */
static bool p_copy(struct driver *d, struct step_wait *w)
{
	const struct fyai_ptyscr_list *c = &d->term.clipboard;
	const char *text = w->st->text, *p;
	size_t i;

	for (i = d->copies_start; i < c->n; i++) {
		for (p = c->v[i]; *p && isspace((unsigned char)*p); p++)
			;
		if (*p && strstr(c->v[i], text))
			return true;
	}
	return false;
}

static bool p_file(struct driver *d, struct step_wait *w)
{
	const struct step *st = w->st;
	char buf[65536];
	size_t n, a = 0, b;
	FILE *fp;

	(void)d;
	fp = fopen(st->text, "r");
	if (!fp)
		return false;
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	buf[n] = '\0';
	if (st->contains)
		return strstr(buf, st->extra) != NULL;
	b = n;
	while (a < b && isspace((unsigned char)buf[a]))
		a++;
	while (b > a && isspace((unsigned char)buf[b - 1]))
		b--;
	return strlen(st->extra) == b - a && !memcmp(buf + a, st->extra, b - a);
}

/*
 * Whether the live input row holds the text. Byte counts cannot say: every
 * earlier appearance (typed echo, queued redisplay, scrollback) is in the
 * capture. Only the screen model sees frames. An empty row behind the marker
 * is the cleared frame between an interrupt and the redisplay, so it does not
 * count as holding.
 */
static bool p_input(struct driver *d, struct step_wait *w)
{
	const char *marker = w->st->extra, *text = w->st->text, *at;
	struct fyai_ptyscr s;
	char *row;
	bool held = false;
	int r;

	if (w->checked == d->len + 1)
		return w->held;
	w->checked = d->len + 1;
	w->held = false;
	row = NULL;
	if (fyai_ptyscr_init(&s, d->rows, d->cols, false))
		return false;
	row = malloc(fyai_ptyscr_row_size(&s));
	if (!row) {
		fyai_ptyscr_destroy(&s);
		return false;
	}
	fyai_ptyscr_feed(&s, d->data, d->len);
	for (r = 0; r < s.rows; r++) {
		fyai_ptyscr_row(&s, r, row);
		at = strstr(row, marker);
		if (!at)
			continue;
		at += strlen(marker);
		while (*at && isspace((unsigned char)*at))
			at++;
		if (!*at) {
			held = false;
			goto out;
		}
		held = strstr(at, text) != NULL;
	}
out:
	free(row);
	fyai_ptyscr_destroy(&s);
	w->held = held;
	return held;
}

static ssize_t write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	ssize_t n;

	while (len) {
		n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* Send keys to the program, and record them. */
static ssize_t send_keys(struct driver *d, const void *buf, size_t len)
{
	fyai_cast_input(d->cast, buf, len);
	return write_all(d->master, buf, len);
}

static int touch_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		return -1;
	close(fd);
	return 0;
}

static double step_deadline(const struct driver *d, const struct step *st);

/* Write the window as two screens compare it to @path. */
static int checkpoint_write(struct driver *d, const char *path)
{
	FILE *f = fopen(path, "w");
	int rc;

	if (!f)
		return -1;
	view_sync(d);
	rc = fyai_ptyscr_dump(&d->view, d->masks, d->nmasks, f);
	if (fclose(f))
		rc = -1;
	return rc;
}

/* Whether the window, as a checkpoint writes it, is the expected screen. */
static bool p_checkpoint(struct driver *d, struct step_wait *w)
{
	char *text = NULL;
	size_t len = 0;
	FILE *f;
	bool same;
	int rc;

	view_sync(d);
	f = open_memstream(&text, &len);
	if (!f)
		return false;
	rc = fyai_ptyscr_dump(&d->view, d->masks, d->nmasks, f);
	if (fclose(f))
		rc = -1;
	same = !rc && len == w->expect_len && !memcmp(text, w->expect, len);
	free(text);
	return same;
}

/*
 * With @expect, the checkpoint of another run, wait until the window is that
 * screen, or until the deadline of the step: a frame that the other run did
 * not show is not a screen that a reader reads. Then write the window to
 * @path in either case, and the runs are compared from the files.
 */
static int checkpoint_step(struct driver *d, const struct step *st)
{
	struct step_wait w = { .st = st };
	FILE *f;
	long size;

	f = st->extra ? fopen(st->extra, "r") : NULL;
	if (f) {
		if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) >= 0 &&
		    !fseek(f, 0, SEEK_SET)) {
			w.expect = malloc((size_t)size + 1);
			if (w.expect &&
			    fread(w.expect, 1, (size_t)size, f) == (size_t)size)
				w.expect_len = (size_t)size;
		}
		fclose(f);
		if (w.expect)
			(void)wait_until(d, p_checkpoint, &w,
					 step_deadline(d, st));
		free(w.expect);
	}
	return checkpoint_write(d, st->text);
}

static int write_file(const char *path, const void *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	int rc;

	if (fd < 0)
		return -1;
	rc = (int)write_all(fd, data, len);
	close(fd);
	return rc;
}

static double step_deadline(const struct driver *d, const struct step *st)
{
	if (st->session)
		return d->session_end;
	return fyai_monotonic_sec() + (st->timeout >= 0 ? st->timeout : d->wait_sec);
}

/* Name a step on the timeline of the recording. */
static void step_marker(struct driver *d, const struct step *st)
{
	char label[160];

	snprintf(label, sizeof(label), "step %zu: %s %.80s", st->index,
		 st->def->name, st->text ? st->text : "");
	fyai_cast_marker(d->cast, label);
}

/*
 * Run one step. A resize is the barrier of the script: the window is painted
 * again whole, so the view reads from here and the rows made for the old size
 * are no longer on it.
 */
static int run_step(struct driver *d, const struct step *st)
{
	struct step_wait w = { .st = st };
	char *tail = NULL, *screen = NULL;
	pred_fn pred = NULL;
	const char *shown;
	double until;
	int row, col, rc;
	bool ok;

	if (d->cast)
		step_marker(d, st);
	switch (st->def->kind) {
	case STEP_TYPE:
	case STEP_WRITE:
	case STEP_CLICK:
	case STEP_RESIZE:
		mark(d);
		break;
	default:
		break;
	}
	switch (st->def->kind) {
	case STEP_TYPE:
		rc = (int)send_keys(d, st->text, st->len);
		if (!rc)
			rc = (int)send_keys(d, "\n", 1);
		fyai_error_check(d->ctx, !rc, err,
				 "step %zu: cannot type: %s", st->index,
				 strerror(errno));
		return 0;
	case STEP_WRITE:
		rc = (int)send_keys(d, st->text, st->len);
		fyai_error_check(d->ctx, !rc, err,
				 "step %zu: cannot write: %s", st->index,
				 strerror(errno));
		return 0;
	case STEP_CLICK:
		/* A position found on the screen does not change with the rows
		 * around it, so wait for the text first. */
		w.st = st;
		until = step_deadline(d, st);
		pred = p_screen;
		wait_until(d, pred, &w, until);
		view_sync(d);
		ok = fyai_ptyscr_locate(&d->view, st->text, &row, &col);
		screen = screen_repr(d);
		fyai_error_check(d->ctx, ok, err,
				 "step %zu: '%s' is not on the screen to click; screen=%s",
				 st->index, st->text, screen ? screen : "");
		{
			char seq[64];

			rc = snprintf(seq, sizeof(seq),
				      "\x1b[<0;%d;%dM\x1b[<0;%d;%dm", col + 1,
				      row + 1, col + 1, row + 1);
			rc = (int)send_keys(d, seq, (size_t)rc);
		}
		fyai_error_check(d->ctx, !rc, err,
				 "step %zu: cannot click: %s", st->index,
				 strerror(errno));
		free(screen);
		return 0;
	case STEP_RESIZE:
		d->resize_rows = st->rows ? st->rows : d->rows;
		d->resize_cols = st->cols;
		set_size(d->master, d->resize_rows, d->resize_cols);
		d->resized = true;
		fyai_cast_resize(d->cast, d->resize_cols, d->resize_rows);
		rc = fyai_ptyscr_resize(&d->term, d->resize_rows,
					d->resize_cols);
		fyai_error_check(d->ctx, !rc, err,
				 "step %zu: out of memory", st->index);
		free(d->held);
		d->held = NULL;
		d->nheld = 0;
		rc = fyai_ptyscr_resize(&d->view, d->resize_rows,
					d->resize_cols);
		fyai_error_check(d->ctx, !rc, err,
				 "step %zu: out of memory", st->index);
		d->view_at = d->len;
		d->scrolled_start = 0;
		d->erased_start = 0;
		d->erases_start = 0;
		return 0;
	case STEP_TOUCH:
		rc = touch_file(st->text);
		fyai_error_check(d->ctx, !rc, err,
				 "step %zu: cannot create %s: %s", st->index,
				 st->text, strerror(errno));
		return 0;
	case STEP_SIGNAL:
		rc = kill(d->pid, (int)st->n);
		fyai_error_check(d->ctx, !rc, err,
				 "step %zu: cannot signal: %s", st->index,
				 strerror(errno));
		return 0;
	case STEP_SNAPSHOT:
		rc = write_file(st->text, d->data, d->len);
		fyai_error_check(d->ctx, !rc, err,
				 "step %zu: cannot write %s: %s", st->index,
				 st->text, strerror(errno));
		return 0;
	case STEP_CHECKPOINT:
		rc = checkpoint_step(d, st);
		fyai_error_check(d->ctx, !rc, err,
				 "step %zu: cannot write %s: %s", st->index,
				 st->text, strerror(errno));
		return 0;
	case STEP_MARK:
		mark(d);
		return 0;
	case STEP_SLEEP: {
		struct timespec ts = { .tv_sec = (time_t)st->seconds,
				       .tv_nsec = (long)((st->seconds -
						(double)(time_t)st->seconds) * 1e9) };

		nanosleep(&ts, NULL);
		return 0;
	}
	case STEP_RESUBMIT: {
		struct step gate = *st, again = *st;
		struct step_wait wc = { .st = &again };
		struct step_wait wg = { .st = &gate };
		long tries = st->n;

		/* Count what arrives after the submit: an earlier turn left
		 * its completion in the capture. */
		again.text = st->extra;
		again.len = strlen(st->extra);
		gate.extra = PROMPT_MARKER;
		wc.target = (long)count_of(d->data, d->len, st->extra) + 1;
		for (;;) {
			rc = (int)send_keys(d, "\n", 1);
			fyai_error_check(d->ctx, !rc, err,
					 "step %zu: cannot submit: %s",
					 st->index, strerror(errno));
			if (wait_until(d, p_count, &wc, d->session_end))
				return 0;
			tail = tail_repr(d);
			fyai_error_check(d->ctx, --tries > 0, err,
					 "step %zu: '%s' did not follow the submit; tail=%s",
					 st->index, st->extra, tail ? tail : "");
			/* The input row must hold the text again before the
			 * next submit; the recall may have been consumed. */
			wg.checked = 0;
			wait_until(d, p_input, &wg, d->session_end);
		}
	}
	default:
		break;
	}

	until = step_deadline(d, st);
	switch (st->def->kind) {
	case STEP_WAIT:
		pred = p_contains;
		break;
	case STEP_WAIT_FRAME:
		pred = p_frame_text;
		break;
	case STEP_WAIT_SCREEN:
		pred = p_screen;
		break;
	case STEP_WAIT_ROW:
		pred = p_row;
		break;
	case STEP_WAIT_GONE:
		pred = p_gone;
		break;
	case STEP_WAIT_REPAINT:
		pred = p_repaint;
		break;
	case STEP_WAIT_COPY:
		pred = p_copy;
		break;
	case STEP_FRAME:
		pred = p_frames;
		break;
	case STEP_WAIT_COUNT:
		pred = p_count;
		w.target = st->more ?
			   (long)count_of(d->data, d->action_start, st->text) +
			   st->n : st->n;
		break;
	case STEP_WAIT_FILE:
		pred = p_file;
		break;
	case STEP_WAIT_INPUT:
		pred = p_input;
		break;
	default:
		fyai_error_check(d->ctx, false, err, "step %zu: unknown kind",
				 st->index);
	}
	if (wait_until(d, pred, &w, until))
		return 0;

	tail = tail_repr(d);
	screen = screen_repr(d);
	switch (st->def->kind) {
	case STEP_FRAME:
		fyai_error(d->ctx, "step %zu: the window painted fewer than %ld frames; tail=%s",
			   st->index, st->n, tail ? tail : "");
		break;
	case STEP_WAIT_COPY: {
		fyai_error(d->ctx, "step %zu: no copy holds '%s'", st->index,
			   st->text);
		break;
	}
	case STEP_WAIT_COUNT:
		fyai_error(d->ctx, "step %zu: the output showed '%s' %zu of %ld times; tail=%s",
			   st->index, st->text,
			   count_of(d->data, d->len, st->text), w.target,
			   tail ? tail : "");
		break;
	case STEP_WAIT_FILE:
		fyai_error(d->ctx, "step %zu: file %s never %s '%s'", st->index,
			   st->text, st->contains ? "contained" : "held",
			   st->extra);
		break;
	case STEP_WAIT_INPUT:
		fyai_error(d->ctx, "step %zu: the input row never held '%s'; tail=%s",
			   st->index, st->text, tail ? tail : "");
		break;
	case STEP_WAIT_SCREEN:
	case STEP_WAIT_ROW:
	case STEP_WAIT_GONE:
	case STEP_WAIT_REPAINT:
		shown = st->def->kind == STEP_WAIT_GONE ? "left" : "reached";
		fyai_error(d->ctx, "step %zu: '%s' never %s the screen; screen=%s",
			   st->index, st->text, shown, screen ? screen : "");
		break;
	default:
		fyai_error(d->ctx, "step %zu: the output never contained '%s'; tail=%s",
			   st->index, st->text, tail ? tail : "");
		break;
	}
err:
	free(tail);
	free(screen);
	return -1;
}

/* ---- the script ---------------------------------------------------- */

static const struct step_def *step_def_find(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(step_defs) / sizeof(step_defs[0]); i++)
		if (!strcmp(step_defs[i].name, name))
			return &step_defs[i];
	return NULL;
}

/*
 * A value of the script names the environment with ${NAME}, so a case need not
 * quote the shell around its text. "$$" is a dollar sign; a "$" that no name
 * follows stands for itself.
 */
static const char *expand_len(struct driver *d, const char *s, size_t slen,
			      size_t index, size_t *olen)
{
	char name[128];
	char *out = malloc(slen + 1), *grown, **owned;
	size_t used = 0, cap = slen + 1, nl, vl, left = slen;
	const char *close, *val;

	fyai_error_check(d->ctx, out, err, "script: out of memory");
	while (left) {
		if (left >= 2 && s[0] == '$' && s[1] == '$') {
			out[used++] = '$';
			s += 2;
			left -= 2;
			continue;
		}
		if (left >= 2 && s[0] == '$' && s[1] == '{' &&
		    (close = memchr(s, '}', left))) {
			nl = (size_t)(close - (s + 2));
			fyai_error_check(d->ctx, nl && nl < sizeof(name), err,
					 "step %zu: bad name in '%.*s'", index,
					 (int)left, s);
			memcpy(name, s + 2, nl);
			name[nl] = '\0';
			val = getenv(name);
			fyai_error_check(d->ctx, val, err,
					 "step %zu: ${%s} is not set", index,
					 name);
			vl = strlen(val);
			left -= (size_t)(close + 1 - s);
			if (used + vl + left + 1 > cap) {
				cap = used + vl + left + 1;
				grown = realloc(out, cap);
				fyai_error_check(d->ctx, grown, err,
						 "script: out of memory");
				out = grown;
			}
			memcpy(out + used, val, vl);
			used += vl;
			s = close + 1;
			continue;
		}
		out[used++] = *s++;
		left--;
	}
	out[used] = '\0';
	owned = realloc(d->owned, (d->nowned + 1) * sizeof(*owned));
	fyai_error_check(d->ctx, owned, err, "script: out of memory");
	d->owned = owned;
	d->owned[d->nowned++] = out;
	if (olen)
		*olen = used;
	return out;
err:
	free(out);
	return NULL;
}

static const char *expand(struct driver *d, const char *s, size_t index)
{
	return expand_len(d, s, strlen(s), index, NULL);
}

/*
 * A string value, expanded. NULL with a diagnostic when it is not one. A
 * string for the program can hold a NUL byte, which Escape followed by NUL
 * needs: the input parser takes that pair as a lone Escape at once.
 */
static const char *value_bytes(struct driver *d, fy_generic v, size_t index,
			       const char *what, size_t *len)
{
	fy_generic copy = v;

	fyai_error_check(d->ctx, fy_is_string(copy), err,
			 "step %zu: %s must be a string", index, what);
	return expand_len(d, fy_castp(&copy, ""), fy_len(copy), index, len);
err:
	return NULL;
}

static const char *value_str(struct driver *d, fy_generic v, size_t index,
			     const char *what)
{
	size_t len;
	const char *text = value_bytes(d, v, index, what, &len);

	fyai_error_check(d->ctx, !text || len == strlen(text), err,
			 "step %zu: %s holds a NUL byte", index, what);
	return text;
err:
	return NULL;
}

/*
 * A number. A case names a value of the shell with ${NAME}, which is a string
 * until it is expanded, so a string that holds only a number is one too.
 */
static bool num_of(struct driver *d, fy_generic v, double *out)
{
	const char *text;
	char *end;

	if (fy_generic_is_int(v)) {
		*out = (double)fy_number(v, 0LL);
		return true;
	}
	if (fy_generic_is_float(v)) {
		*out = fy_number(v, 0.0);
		return true;
	}
	if (!fy_is_string(v))
		return false;
	text = expand(d, fy_castp(&v, ""), 0);
	if (!text || !*text)
		return false;
	*out = strtod(text, &end);
	return !*end;
}

static int signal_number(const char *name)
{
	static const struct { const char *name; int sig; } names[] = {
		{ "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT },
		{ "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
		{ "TERM", SIGTERM }, { "WINCH", SIGWINCH },
	};
	size_t i;

	if (!strncmp(name, "SIG", 3))
		name += 3;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(names[i].name, name))
			return names[i].sig;
	return 0;
}

static int parse_resize(struct driver *d, struct step *st, fy_generic v)
{
	const char *text, *x;
	double num;
	char *end;
	long a, b;

	if (num_of(d, v, &num)) {
		fyai_error_check(d->ctx, num >= 1, err,
				 "step %zu: resize needs a positive width",
				 st->index);
		st->cols = (int)num;
		return 0;
	}
	text = value_str(d, v, st->index, "resize");
	if (!text)
		return -1;
	x = strchr(text, 'x');
	a = strtol(text, &end, 10);
	fyai_error_check(d->ctx, end != text && a > 0, err,
			 "step %zu: resize is COLS or ROWSxCOLS, not '%s'",
			 st->index, text);
	if (!x) {
		fyai_error_check(d->ctx, !*end, err,
				 "step %zu: resize is COLS or ROWSxCOLS, not '%s'",
				 st->index, text);
		st->cols = (int)a;
		return 0;
	}
	fyai_error_check(d->ctx, end == x, err,
			 "step %zu: resize is COLS or ROWSxCOLS, not '%s'",
			 st->index, text);
	b = strtol(x + 1, &end, 10);
	fyai_error_check(d->ctx, !*end && b > 0 && x[1], err,
			 "step %zu: resize is COLS or ROWSxCOLS, not '%s'",
			 st->index, text);
	st->rows = (int)a;
	st->cols = (int)b;
	return 0;
err:
	return -1;
}

static int step_parse(struct driver *d, fy_generic step, size_t index,
		      struct step *st)
{
	fy_generic key, value;
	const struct step_def *def = NULL;
	fy_generic main_value = fy_invalid;
	unsigned int seen = 0, bit;
	const char *name, *text;
	double num;

	memset(st, 0, sizeof(*st));
	st->index = index;
	st->timeout = -1.0;
	st->n = 1;
	fyai_error_check(d->ctx, fy_is_mapping(step), err,
			 "step %zu: a step is a mapping", index);
	fy_foreach_key_value(key, value, step) {
		fyai_error_check(d->ctx, fy_is_string(key), err,
				 "step %zu: a key is a string", index);
		name = fy_castp(&key, "");
		if (step_def_find(name)) {
			fyai_error_check(d->ctx, !def, err,
					 "step %zu: '%s' follows the kind '%s'",
					 index, name, def->name);
			def = step_def_find(name);
			main_value = value;
			continue;
		}
		bit = !strcmp(name, "timeout") ? MOD_TIMEOUT :
		      !strcmp(name, "count") ? MOD_COUNT :
		      !strcmp(name, "more") ? MOD_MORE :
		      !strcmp(name, "equals") ? MOD_EQUALS :
		      !strcmp(name, "contains") ? MOD_CONTAINS :
		      !strcmp(name, "marker") ? MOD_MARKER :
		      !strcmp(name, "from") ? MOD_FROM :
		      !strcmp(name, "expect") ? MOD_EXPECT :
		      !strcmp(name, "tries") ? MOD_TRIES : 0;
		fyai_error_check(d->ctx, bit, err,
				 "step %zu: unknown key '%s'", index, name);
		seen |= bit;
	}
	fyai_error_check(d->ctx, def, err, "step %zu: it names no step kind",
			 index);
	fyai_error_check(d->ctx, !(seen & ~def->mods), err,
			 "step %zu: '%s' does not take that modifier", index,
			 def->name);
	fyai_error_check(d->ctx, !def->need || (seen & def->need), err,
			 "step %zu: '%s' needs a modifier", index, def->name);
	st->def = def;

	switch (def->kind) {
	case STEP_SNAPSHOT:
	case STEP_CHECKPOINT:
	case STEP_TOUCH:
	case STEP_WAIT_FILE:
		text = value_str(d, main_value, index, def->name);
		if (!text)
			return -1;
		break;
	case STEP_MARK:
		text = "";
		break;
	case STEP_SLEEP:
		fyai_error_check(d->ctx, num_of(d, main_value, &num) &&
				 num >= 0, err,
				 "step %zu: sleep is a number of seconds",
				 index);
		st->seconds = num;
		text = "";
		break;
	case STEP_FRAME:
		fyai_error_check(d->ctx, num_of(d, main_value, &num) &&
				 num >= 1, err,
				 "step %zu: frame is a count of frames", index);
		st->n = (long)num;
		text = "";
		break;
	case STEP_SIGNAL:
		if (num_of(d, main_value, &num)) {
			st->n = (long)num;
		} else {
			text = value_str(d, main_value, index, "signal");
			if (!text)
				return -1;
			st->n = signal_number(text);
		}
		fyai_error_check(d->ctx, st->n >= 1, err,
				 "step %zu: signal is a number or a name",
				 index);
		text = "";
		break;
	case STEP_RESIZE:
		if (parse_resize(d, st, main_value))
			return -1;
		text = "";
		break;
	case STEP_TYPE:
	case STEP_WRITE:
		text = value_bytes(d, main_value, index, def->name, &st->len);
		if (!text)
			return -1;
		st->text = text;
		goto modifiers;
	default:
		text = value_str(d, main_value, index, def->name);
		if (!text)
			return -1;
		fyai_error_check(d->ctx, *text || def->kind == STEP_WAIT_COPY ||
				 def->kind == STEP_TYPE || def->kind == STEP_WRITE,
				 err, "step %zu: '%s' needs a text", index,
				 def->name);
		break;
	}
	st->text = text;
	st->len = strlen(text);
modifiers:
	fy_foreach_key_value(key, value, step) {
		name = fy_castp(&key, "");
		if (step_def_find(name))
			continue;
		if (!strcmp(name, "timeout")) {
			if (fy_is_string(value)) {
				fyai_error_check(d->ctx,
						 fy_equal(value, "session"), err,
						 "step %zu: timeout is seconds or 'session'",
						 index);
				st->session = true;
			} else {
				fyai_error_check(d->ctx, num_of(d, value, &num) &&
						 num > 0, err,
						 "step %zu: timeout is seconds or 'session'",
						 index);
				st->timeout = num * d->scale;
			}
		} else if (!strcmp(name, "count") || !strcmp(name, "more") ||
			   !strcmp(name, "tries")) {
			fyai_error_check(d->ctx, num_of(d, value, &num) &&
					 num >= 1, err,
					 "step %zu: %s is a positive number",
					 index, name);
			st->n = (long)num;
			st->more = !strcmp(name, "more");
		} else if (!strcmp(name, "from")) {
			fyai_error_check(d->ctx, fy_equal(value, "start") ||
					 fy_equal(value, "mark"), err,
					 "step %zu: from is 'start' or 'mark'",
					 index);
			st->from_start = fy_equal(value, "start");
		} else {
			st->extra = value_str(d, value, index, name);
			if (!st->extra)
				return -1;
			st->contains = !strcmp(name, "contains");
		}
	}
	if (def->kind == STEP_WAIT_INPUT && !st->extra)
		st->extra = PROMPT_MARKER;
	if (def->kind == STEP_RESUBMIT && !(seen & MOD_TRIES))
		st->n = 3;
	if (def->kind == STEP_RESUBMIT)
		st->session = true;
	return 0;
err:
	return -1;
}

/* The patterns of "mask": extended regular expressions, one for each item. */
static int masks_load(struct driver *d, fy_generic list)
{
	fy_generic item;
	const char *text;
	size_t n = 0;
	int rc;

	if (fy_is_invalid(list))
		return 0;
	fyai_error_check(d->ctx, fy_is_sequence(list), err,
			 "script: mask is a list of patterns");
	d->masks = calloc(fy_len(list) + 1, sizeof(*d->masks));
	fyai_error_check(d->ctx, d->masks, err, "script: cannot keep the masks");
	fy_foreach(item, list) {
		text = value_str(d, item, 0, "mask");
		if (!text)
			return -1;
		rc = regcomp(&d->masks[n], text, REG_EXTENDED);
		fyai_error_check(d->ctx, !rc, err,
				 "script: mask '%s' is not a pattern", text);
		d->nmasks = ++n;
	}
	return 0;
err:
	return -1;
}

static int script_load(struct driver *d, const char *path)
{
	static const char *const keys[] = {
		"rows", "cols", "timeout", "wait_timeout", "ready",
		"background", "background_while_starting", "emoji_vs",
		"snapshot", "mask", "exit", "steps",
	};
	fy_generic doc, key, value, steps, exit, v;
	const char *name, *text;
	double num;
	size_t i, k;
	bool known;

	d->rows = 30;
	d->cols = 100;
	d->session_sec = SESSION_DEFAULT * d->scale;
	d->wait_sec = WAIT_DEFAULT * d->scale;
	d->ready = READY_DEFAULT;
	doc = fy_parse_file(d->gb, FYAI_YAML_PARSE_FLAGS, path);
	fyai_error_check(d->ctx, fy_is_mapping(doc), err,
			 "script: cannot parse %s", path);
	fy_foreach_key_value(key, value, doc) {
		name = fy_castp(&key, "");
		for (known = false, k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
			if (!strcmp(keys[k], name))
				known = true;
		fyai_error_check(d->ctx, known, err,
				 "script: unknown key '%s'", name);
	}
	v = fy_get(doc, "rows", fy_invalid);
	if (fy_is_valid(v)) {
		fyai_error_check(d->ctx, num_of(d, v, &num) && num >= 1, err,
				 "script: rows is a positive number");
		d->rows = (int)num;
	}
	v = fy_get(doc, "cols", fy_invalid);
	if (fy_is_valid(v)) {
		fyai_error_check(d->ctx, num_of(d, v, &num) && num >= 1, err,
				 "script: cols is a positive number");
		d->cols = (int)num;
	}
	v = fy_get(doc, "timeout", fy_invalid);
	if (fy_is_valid(v)) {
		fyai_error_check(d->ctx, num_of(d, v, &num) && num > 0, err,
				 "script: timeout is a number of seconds");
		d->session_sec = num * d->scale;
	}
	v = fy_get(doc, "wait_timeout", fy_invalid);
	if (fy_is_valid(v)) {
		fyai_error_check(d->ctx, num_of(d, v, &num) && num > 0, err,
				 "script: wait_timeout is a number of seconds");
		d->wait_sec = num * d->scale;
	}
	v = fy_get(doc, "ready", fy_invalid);
	if (fy_is_valid(v)) {
		text = value_str(d, v, 0, "ready");
		if (!text)
			return -1;
		fyai_error_check(d->ctx, *text, err,
				 "script: ready needs a text");
		d->ready = text;
	}
	v = fy_get(doc, "background", fy_invalid);
	if (fy_is_valid(v)) {
		d->background = value_str(d, v, 0, "background");
		if (!d->background)
			return -1;
	}
	d->background_starting = fy_get(doc, "background_while_starting",
					false);
	v = fy_get(doc, "emoji_vs", fy_invalid);
	if (fy_is_valid(v)) {
		fyai_error_check(d->ctx, fy_equal(v, "narrow"), err,
				 "script: emoji_vs is 'narrow'");
		d->emoji_narrow = true;
	}
	v = fy_get(doc, "snapshot", fy_invalid);
	if (fy_is_valid(v)) {
		d->snapshot = value_str(d, v, 0, "snapshot");
		if (!d->snapshot)
			return -1;
	}
	if (masks_load(d, fy_get(doc, "mask", fy_invalid)))
		return -1;
	exit = fy_get(doc, "exit", fy_invalid);
	if (fy_is_valid(exit)) {
		fyai_error_check(d->ctx, fy_is_mapping(exit), err,
				 "script: exit is a mapping");
		fy_foreach_key_value(key, value, exit) {
			name = fy_castp(&key, "");
			fyai_error_check(d->ctx, !strcmp(name, "status") ||
					 !strcmp(name, "signal") ||
					 !strcmp(name, "clear"), err,
					 "script: unknown key 'exit/%s'", name);
		}
		d->exit_status = (int)fy_number(fy_get(exit, "status",
						       fy_invalid), 0LL);
		v = fy_get(exit, "signal", fy_invalid);
		if (fy_is_valid(v)) {
			if (num_of(d, v, &num)) {
				d->exit_signal = (int)num;
			} else {
				text = value_str(d, v, 0, "exit/signal");
				if (!text)
					return -1;
				d->exit_signal = signal_number(text);
			}
			fyai_error_check(d->ctx, d->exit_signal >= 1, err,
					 "script: exit/signal is a number or a name");
		}
		d->exit_clear = fy_get(exit, "clear", false);
	}
	steps = fy_get(doc, "steps", fy_seq_empty);
	fyai_error_check(d->ctx, fy_is_sequence(steps), err,
			 "script: steps is a sequence");
	d->nsteps = fy_len(steps);
	d->steps = calloc(d->nsteps + 1, sizeof(*d->steps));
	fyai_error_check(d->ctx, d->steps, err, "script: out of memory");
	for (i = 0; i < d->nsteps; i++)
		if (step_parse(d, fy_get_at(steps, i), i + 1, &d->steps[i]))
			return -1;
	return 0;
err:
	return -1;
}

/* ---- the session --------------------------------------------------- */

static int session_start(struct driver *d, char *const *argv,
			 char *const *env)
{
	static const int reset[] = { SIGINT, SIGTERM, SIGQUIT,
				     SIGHUP, SIGALRM, SIGCHLD, SIGWINCH };
	int master, slave, rc;
	const char *name;
	struct rlimit rl;
	sigset_t none;
	size_t i;
	pid_t pid;

	master = posix_openpt(O_RDWR | O_NOCTTY);
	fyai_error_check(d->ctx, master >= 0, err,
			 "pty: cannot open a pseudo-terminal: %s",
			 strerror(errno));
	rc = grantpt(master);
	if (!rc)
		rc = unlockpt(master);
	fyai_error_check(d->ctx, !rc, err_close,
			 "pty: cannot unlock the pseudo-terminal: %s",
			 strerror(errno));
	name = ptsname(master);
	fyai_error_check(d->ctx, name, err_close,
			 "pty: no name for the pseudo-terminal");
	slave = open(name, O_RDWR | O_NOCTTY);
	fyai_error_check(d->ctx, slave >= 0, err_close,
			 "pty: cannot open %s: %s", name, strerror(errno));
	set_size(slave, d->rows, d->cols);
	pid = fork();
	fyai_error_check(d->ctx, pid >= 0, err_slave, "pty: cannot fork: %s",
			 strerror(errno));
	if (!pid) {
		/* A controlling terminal with a foreground process group. */
		setsid();
		ioctl(slave, TIOCSCTTY, 0);
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);
		fyai_close_fds_from(3);
		if (!getrlimit(RLIMIT_STACK, &rl) && rl.rlim_cur != RLIM_INFINITY &&
		    rl.rlim_cur > STACK_LIMIT) {
			rl.rlim_cur = STACK_LIMIT;
			setrlimit(RLIMIT_STACK, &rl);
		}
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		for (i = 0; i < sizeof(reset) / sizeof(reset[0]); i++)
			signal(reset[i], SIG_DFL);
		/* The program ends a session with the terminal gone: a write
		 * to it must fail, not kill the program. */
		signal(SIGPIPE, SIG_IGN);
		setenv("TERM", "xterm-256color", 0);
		/* Left by the stack re-exec of this process. */
		unsetenv("STACK_LIMIT_RAISED");
		for (i = 0; env[i]; i++)
			putenv(env[i]);
		execv(argv[0], argv);
		_exit(127);
	}
	close(slave);
	d->master = master;
	d->pid = pid;
	return 0;
err_slave:
	close(slave);
err_close:
	close(master);
err:
	return -1;
}

static void session_kill(struct driver *d)
{
	double until;
	pid_t done;
	int status;

	if (d->reaped)
		return;
	/* SIGTERM first: fyai ends the sessions it serves. A process that does
	 * not end by the deadline is killed. */
	kill(d->pid, SIGTERM);
	until = fyai_monotonic_sec() + TERM_GRACE_SEC * d->scale;
	while (fyai_monotonic_sec() < until) {
		done = waitpid(d->pid, &status, WNOHANG);
		if (done) {
			d->reaped = true;
			return;
		}
		if (!d->eof && pump(d, 20) < 0)
			d->eof = true;
		else if (d->eof) {
			struct timespec ts = { .tv_nsec = 20 * 1000000L };

			nanosleep(&ts, NULL);
		}
	}
	kill(d->pid, SIGKILL);
	waitpid(d->pid, &status, 0);
	d->reaped = true;
}

static int session_exit(struct driver *d)
{
	struct timespec ts = { .tv_nsec = 5 * 1000000L };
	pid_t done;
	int status, rc;

	/* A program that ends on its own leaves no terminal to write to. The
	 * exit line asks a session that still runs to leave; no step depends
	 * on it. */
	fyai_cast_marker(d->cast, "exit");
	if (d->exit_clear) {
		fyai_cast_input(d->cast, "\x15", 1);
		write_quiet(d->master, "\x15", 1);
	}
	fyai_cast_input(d->cast, "/exit\n", 6);
	write_quiet(d->master, "/exit\n", 6);
	while (fyai_monotonic_sec() < d->session_end) {
		if (!d->eof)
			pump(d, POLL_MS);
		else
			nanosleep(&ts, NULL);
		done = waitpid(d->pid, &status, WNOHANG);
		if (!done)
			continue;
		d->reaped = true;
		if (WIFEXITED(status) && !d->exit_signal &&
		    WEXITSTATUS(status) == d->exit_status)
			return 0;
		if (WIFSIGNALED(status) && d->exit_signal &&
		    WTERMSIG(status) == d->exit_signal)
			return 0;
		rc = WIFSIGNALED(status) ? WTERMSIG(status) :
		     WIFEXITED(status) ? WEXITSTATUS(status) : status;
		fyai_error(d->ctx, "the program %s %d, not as the script expects",
			   WIFSIGNALED(status) ? "died from signal" :
			   "exited with status", rc);
		return -1;
	}
	fyai_error(d->ctx, "the program did not exit");
	return -1;
}

static int session_run(struct driver *d)
{
	char *tail;
	size_t i;
	int rc;

	d->session_end = fyai_monotonic_sec() + d->session_sec;
	rc = 0;
	{
		struct step ready = { .text = d->ready, .session = true,
				      .timeout = -1.0 };
		struct step_wait w = { .st = &ready };

		ready.len = strlen(d->ready);
		/* Wait until the first synchronized update has made the input
		 * cursor visible. A fixed pause races the sanitizers and slow
		 * runners, and the tty echoes input before the UI enters raw
		 * mode. A session that opens with a tile that holds the keys
		 * shows no prompt cursor: the script then states the text. */
		if (!wait_until(d, p_contains, &w, d->session_end)) {
			tail = tail_repr(d);
			fyai_error(d->ctx, "the output never contained the ready text %s; tail=%s",
				   d->ready, tail ? tail : "");
			free(tail);
			return -1;
		}
	}
	if (d->background_starting)
		d->background = NULL;
	for (i = 0; i < d->nsteps && !rc; i++)
		rc = run_step(d, &d->steps[i]);
	if (rc)
		return -1;
	if (d->snapshot && !d->snapshot_taken)
		if (write_file(d->snapshot, d->data, d->len)) {
			fyai_error(d->ctx, "cannot write %s: %s", d->snapshot,
				   strerror(errno));
			return -1;
		}
	return session_exit(d);
}

static void driver_free(struct driver *d)
{
	size_t i;

	fyai_cast_close(d->cast);
	for (i = 0; i < d->nowned; i++)
		free(d->owned[i]);
	free(d->owned);
	for (i = 0; i < d->nmasks; i++)
		regfree(&d->masks[i]);
	free(d->masks);
	free(d->steps);
	free(d->data);
	free(d->held);
	fyai_ptyscr_destroy(&d->term);
	fyai_ptyscr_destroy(&d->view);
}

int fyai_ptydrv_run(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		    const char *script, const char *output,
		    char *const *argv, char *const *env, const char *cast)
{
	struct driver d = { .ctx = ctx, .gb = gb, .master = -1 };
	int rc, failed;

	d.scale = timeout_scale();
	if (script_load(&d, script))
		goto out;
	rc = fyai_ptyscr_init(&d.term, d.rows, d.cols, d.emoji_narrow);
	rc |= fyai_ptyscr_init(&d.view, d.rows, d.cols, d.emoji_narrow);
	fyai_error_check(ctx, !rc, out, "pty: out of memory");
	/* The snapshot steps decide if the end snapshot is wanted. */
	{
		size_t i;

		for (i = 0; i < d.nsteps; i++)
			if (d.steps[i].def->kind == STEP_SNAPSHOT)
				d.snapshot_taken = true;
	}
	if (cast) {
		d.cast = fyai_cast_open(ctx, cast, d.cols, d.rows, argv[0], fy_invalid);
		if (!d.cast)
			goto out;
	}
	if (session_start(&d, argv, env))
		goto out;
	failed = session_run(&d);
	if (write_file(output, d.data, d.len)) {
		fyai_error(ctx, "cannot write %s: %s", output, strerror(errno));
		failed = -1;
	}
	session_kill(&d);
	close(d.master);
	driver_free(&d);
	return failed ? -1 : 0;
out:
	driver_free(&d);
	return -1;
}
