/*
 * fyai_cast.c - asciicast v2 recording
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A recording is one JSON header line and one JSON array for each event:
 * [seconds, "o" | "i" | "r" | "m", data]. The lines are the documents of one
 * stream of a libfyaml emitter in JSON mode. Each value is a generic, which
 * the generic iterator turns into events for the emitter; the events of the
 * containers are made here. The format needs valid UTF-8 text, thus the writer
 * repairs the bytes it gets before the emitter escapes them.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libfyaml.h>

#include "fyai.h"
#include "fyai_cast.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* The longest UTF-8 character. */
#define CAST_UTF8_MAX 4

#define CAST_REPLACEMENT "\xEF\xBF\xBD"

struct fyai_cast {
	struct fyai_ctx *ctx;
	FILE *fp;
	struct fy_emitter *emit;
	struct fy_generic_iterator *gi;
	double t0;
	bool failed;
	/* The repaired text of the event that is made. */
	struct fyai_buf text;
	/* The start of a character that a write cut: per kind of event. */
	unsigned char hold[2][CAST_UTF8_MAX];
	size_t nhold[2];
	/* A synchronized update that has begun and not ended: held until it ends. */
	struct fyai_buf block;
	bool in_sync;
	/* The body of the last update that only put the cursor, as it was recorded. */
	char *noop;
};

/* Add the repaired text of @in to the text of the event; keep a cut character. */
static int put_text(struct fyai_cast *c, int kind, const void *in, size_t n)
{
	unsigned char buf[CAST_UTF8_MAX];
	const unsigned char *p = in;
	size_t i = 0, used, len;
	ssize_t take;
	int rc = 0;

	/* The held start joins the bytes that follow. */
	if (c->nhold[kind]) {
		len = c->nhold[kind];
		memcpy(buf, c->hold[kind], len);
		used = n < CAST_UTF8_MAX - len ? n : CAST_UTF8_MAX - len;
		memcpy(buf + len, p, used);
		take = fyai_utf8_scan(buf, len + used);
		if (take < 0) {
			memcpy(c->hold[kind], buf, len + used);
			c->nhold[kind] = len + used;
			return 0;
		}
		c->nhold[kind] = 0;
		if (take > 0) {
			rc |= fyai_buf_append_data(&c->text, buf, take);
			i = (size_t)take - len;
		} else {
			/* The bytes after the lead start again. */
			rc |= fyai_buf_append_data(&c->text,
							  CAST_REPLACEMENT, 3);
		}
	}
	while (i < n && !rc) {
		take = fyai_utf8_scan(p + i, n - i);
		if (take < 0) {
			/* A prefix that can still become a character is held. */
			c->nhold[kind] = n - i;
			memcpy(c->hold[kind], p + i, n - i);
			return rc;
		}
		if (take > 0) {
			rc |= fyai_buf_append_data(&c->text, p + i, take);
			i += take;
		} else {
			rc |= fyai_buf_append_data(&c->text,
							  CAST_REPLACEMENT, 3);
			i++;
		}
	}
	return rc;
}

static int emitter_output(struct fy_emitter *emit, enum fy_emitter_write_type type,
			  const char *str, int len, void *user)
{
	struct fyai_cast *c = user;

	(void)emit;
	(void)type;
	return fwrite(str, 1, (size_t)len, c->fp) == (size_t)len ? len : -1;
}

/* Write @v as one document, and the line with it. */
static void line_write(struct fyai_cast *c, fy_generic v)
{
	struct fy_event *fye;
	int rc = 0;

	rc = fy_emit_eventf(c->emit, FYET_DOCUMENT_START, 1, NULL, NULL);
	fy_generic_iterator_generic_start(c->gi, v);
	while (!rc && (fye = fy_generic_iterator_body_next(c->gi)))
		rc = fy_emit_event(c->emit, fye);
	if (!rc && fy_generic_iterator_get_error(c->gi))
		rc = -1;
	if (!rc)
		rc = fy_emit_eventf(c->emit, FYET_DOCUMENT_END, 1);
	if (!rc && fflush(c->fp))
		rc = -1;
	if (rc && !c->failed) {
		c->failed = true;
		fyai_warning(c->ctx, "cast: cannot write the recording: %s",
			     errno ? strerror(errno) : "emitter error");
	}
}

/* One event: [time, kind, text]; @text is the repaired bytes of the event. */
static void event(struct fyai_cast *c, const char *kind, int hold,
		  const void *data, size_t len)
{
	const char *text;
	size_t ntext;
	double t;
	int rc;

	if (!c || c->failed)
		return;
	/* Microseconds: the digits of a double below that are noise. */
	t = round((fyai_monotonic_sec() - c->t0) * 1e6) / 1e6;
	c->text.len = 0;
	rc = put_text(c, hold, data, len);
	if (rc) {
		fyai_warning(c->ctx, "cast: out of memory");
		c->failed = true;
		return;
	}
	text = c->text.data ? c->text.data : "";
	ntext = c->text.len;
	line_write(c, fy_sequence(t, kind, fy_string_size(text, ntext)));
}

#define CAST_SYNC_BEGIN "\x1b[?2026h"
#define CAST_SYNC_END "\x1b[?2026l"
#define CAST_SYNC_LEN 8
/* A held update that grows past this is recorded as it is. */
#define CAST_SYNC_MAX (1024 * 1024)

/*
 * Whether the body of an update only places the cursor and shows or hides it:
 * a position, then private mode 25. The display writes such an update at each
 * wake-up of an idle page, and it changes nothing the viewer sees.
 */
static bool cast_body_is_cursor(const char *body, size_t len)
{
	size_t i = 0;

	if (i + 2 < len && body[i] == '\x1b' && body[i + 1] == '[') {
		size_t j = i + 2;

		while (j < len && ((body[j] >= '0' && body[j] <= '9') || body[j] == ';'))
			j++;
		if (j < len && body[j] == 'H')
			i = j + 1;
	}
	while (i < len) {
		if (len - i >= 6 && !memcmp(body + i, "\x1b[?25", 5) &&
		    (body[i + 5] == 'h' || body[i + 5] == 'l'))
			i += 6;
		else
			return false;
	}
	return true;
}

/* Record a held update, or drop one that repeats the last cursor update. */
static void cast_sync_done(struct fyai_cast *c, const char *block, size_t len)
{
	const char *body = block + CAST_SYNC_LEN;
	size_t blen = len - 2 * CAST_SYNC_LEN;
	bool cursor = cast_body_is_cursor(body, blen);

	if (cursor && c->noop && strlen(c->noop) == blen && !memcmp(c->noop, body, blen))
		return;
	free(c->noop);
	c->noop = cursor ? strndup(body, blen) : NULL;
	event(c, "o", 0, block, len);
}

void fyai_cast_output(struct fyai_cast *c, const void *data, size_t len)
{
	const char *p = data, *b, *e;
	size_t used;

	if (!c || c->failed || !data)
		return;
	while (len) {
		if (!c->in_sync) {
			b = memmem(p, len, CAST_SYNC_BEGIN, CAST_SYNC_LEN);
			if (!b) {
				free(c->noop);
				c->noop = NULL;
				event(c, "o", 0, p, len);
				return;
			}
			if (b > p) {
				free(c->noop);
				c->noop = NULL;
				event(c, "o", 0, p, (size_t)(b - p));
			}
			len -= (size_t)(b - p);
			p = b;
			c->in_sync = true;
			c->block.len = 0;
		}
		/* Look for the end where it can start: the held part and this chunk. */
		used = c->block.len;
		if (fyai_buf_append_data(&c->block, p, len)) {
			fyai_warning(c->ctx, "cast: out of memory");
			c->failed = true;
			return;
		}
		e = memmem(c->block.data + (used > CAST_SYNC_LEN ? used - CAST_SYNC_LEN : 0),
			   c->block.len - (used > CAST_SYNC_LEN ? used - CAST_SYNC_LEN : 0),
			   CAST_SYNC_END, CAST_SYNC_LEN);
		if (!e) {
			if (c->block.len > CAST_SYNC_MAX) {
				c->in_sync = false;
				free(c->noop);
				c->noop = NULL;
				event(c, "o", 0, c->block.data, c->block.len);
			}
			return;
		}
		e += CAST_SYNC_LEN;
		/* What follows the end of the update is the start of the next output. */
		used = c->block.len - (size_t)(e - c->block.data);
		cast_sync_done(c, c->block.data, (size_t)(e - c->block.data));
		c->in_sync = false;
		p = e;
		len = used;
		/* The tail is in the block buffer: keep a copy that the loop can read. */
		if (len) {
			char *tail = malloc(len);

			if (!tail) {
				fyai_warning(c->ctx, "cast: out of memory");
				c->failed = true;
				return;
			}
			memcpy(tail, p, len);
			fyai_cast_output(c, tail, len);
			free(tail);
		}
		return;
	}
}

void fyai_cast_input(struct fyai_cast *c, const void *data, size_t len)
{
	event(c, "i", 1, data, len);
}

void fyai_cast_resize(struct fyai_cast *c, int cols, int rows)
{
	const char *size = fy_sprintfa("%dx%d", cols, rows);

	event(c, "r", 0, size, strlen(size));
}

void fyai_cast_marker(struct fyai_cast *c, const char *label)
{
	event(c, "m", 0, label, strlen(label));
}

/* The header: {version, width, height, timestamp, command}. */
static int header_write(struct fyai_cast *c, int cols, int rows,
			const char *command, fy_generic props)
{
	long long now = (long long)time(NULL);
	fy_generic head;

	head = fy_mapping("version", 2, "width", cols, "height", rows,
			  "timestamp", now);
	if (command)
		head = fy_assoc(head, "command", command);
	if (fy_is_mapping(props))
		head = fy_assoc(head, "fyai", props);
	line_write(c, head);
	return c->failed ? -1 : 0;
}

void fyai_cast_close(struct fyai_cast *c)
{
	if (!c)
		return;
	/* An update that never ended is recorded as it is. */
	if (c->in_sync && c->block.len && !c->failed) {
		c->in_sync = false;
		event(c, "o", 0, c->block.data, c->block.len);
	}
	/* A character that was never finished is a replacement. */
	if (c->nhold[0] && !c->failed) {
		c->nhold[0] = 0;
		event(c, "o", 0, CAST_REPLACEMENT, 3);
	}
	if (c->emit) {
		if (!c->failed)
			(void)fy_emit_eventf(c->emit, FYET_STREAM_END);
		fy_emitter_destroy(c->emit);
	}
	fy_generic_iterator_destroy(c->gi);
	if (c->fp)
		fclose(c->fp);
	free(c->text.data);
	free(c->block.data);
	free(c->noop);
	free(c);
}

struct fyai_cast *fyai_cast_open(struct fyai_ctx *ctx, const char *path,
				 int cols, int rows, const char *command,
				 fy_generic props)
{
	struct fy_emitter_cfg ecfg;
	struct fyai_cast *c = calloc(1, sizeof(*c));
	int fd = -1, op_rc;

	fyai_error_check(ctx, c, err, "cast: out of memory");
	c->ctx = ctx;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	fyai_error_check(ctx, fd >= 0, err, "cast: cannot create %s: %s", path,
			 strerror(errno));
	c->fp = fdopen(fd, "w");
	fyai_error_check(ctx, c->fp, err, "cast: cannot open %s: %s", path,
			 strerror(errno));
	fd = -1;
	c->gi = fy_generic_iterator_create();
	fyai_error_check(ctx, c->gi, err,
			 "cast: cannot make the generic iterator");
	memset(&ecfg, 0, sizeof(ecfg));
	ecfg.flags = FYECF_MODE_JSON_ONELINE | FYECF_WIDTH_INF;
	ecfg.output = emitter_output;
	ecfg.userdata = c;
	c->emit = fy_emitter_create(&ecfg);
	fyai_error_check(ctx, c->emit, err, "cast: cannot make the emitter");
	op_rc = fy_emit_eventf(c->emit, FYET_STREAM_START);
	fyai_error_check(ctx, !op_rc, err,
			 "cast: cannot start the stream");
	c->t0 = fyai_monotonic_sec();
	op_rc = header_write(c, cols, rows, command, props);
	fyai_error_check(ctx, !op_rc, err,
			 "cast: cannot write %s", path);
	return c;
err:
	if (fd >= 0)
		close(fd);
	c = c ? (fyai_cast_close(c), NULL) : NULL;
	return c;
}
