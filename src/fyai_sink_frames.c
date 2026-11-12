/* SPDX-License-Identifier: MIT */
/*
 * fyai_sink_frames.c - the sink of a remote command
 *
 * A command that a client runs with --remote runs at a server with this
 * backend (--sink-frames). It presents nothing: it sends each operation of the
 * sink as a frame on standard output, and the client presents the frame with
 * its own sink, at its own width. The first frame gives the display settings
 * of the worker, so the client renders as a local run of the command would.
 *
 *   {type: fyai, subtype: display, config}
 *   {type: fyai, subtype: sink, op, stream?, data?, kind?, aborted?}
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_config.h"
#include "fyai_markdown.h"
#include "fyai_remote.h"
#include "fyai_scratch.h"
#include "fyai_sink.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_REMOTE

struct sink_frames {
	struct fy_generic_builder *gb;	/* reset when a frame is sent */
	/* The display frame goes while the caller still holds the arguments
	 * of its own frame in @gb, which a reset would free. */
	struct fy_generic_builder *greet_gb;
	int fd;
	bool greeted;			/* the display frame went */
	bool failed;			/* reported once */
};

static const char *const frame_streams[] = {
	[FYAI_SINK_TRANSCRIPT]	= "transcript",
	[FYAI_SINK_NOTICE]	= "notice",
	[FYAI_SINK_STATUS]	= "status",
	[FYAI_SINK_DIAG]	= "diag",
	[FYAI_SINK_MACHINE]	= "machine",
};

static const char *const frame_kinds[] = {
	[FYAI_SINK_DOC_SYSTEM]		= "system",
	[FYAI_SINK_DOC_USER]		= "user",
	[FYAI_SINK_DOC_ASSISTANT]	= "assistant",
};

/*
 * Write @frame and a newline to the descriptor of @s; -1 when it cannot. The
 * text of the frame is made in @gb, which the caller resets.
 */
static int frames_send(struct fyai_sink *s, fy_generic frame)
{
	struct sink_frames *f = s->state;
	const char *text;
	size_t len, done = 0;
	ssize_t n;

	if (f->failed)
		return -1;
	text = emit_json_string(f->gb, frame);
	if (!text)
		goto err;
	len = strlen(text);
	while (done <= len) {
		/* The newline that ends the frame is the byte after the text. */
		if (done == len)
			n = write(f->fd, "\n", 1);
		else
			n = write(f->fd, text + done, len - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			goto err;
		done += (size_t)n;
	}
	return 0;
err:
	f->failed = true;
	fyai_error(s->ctx, "cannot send the output to the client: %s",
		   strerror(errno));
	return -1;
}

/* Send @frame, which is in the builder of the sink, and release the builder. */
static int frames_put(struct fyai_sink *s, fy_generic frame)
{
	struct sink_frames *f = s->state;
	int rc;

	rc = frames_send(s, frame);
	fy_generic_builder_reset(f->gb);
	return rc;
}

/* Send the display frame before the first output. */
static int frames_greet(struct fyai_sink *s)
{
	struct sink_frames *f = s->state;
	fy_generic display, frame;
	int rc;

	if (f->greeted)
		return 0;
	f->greeted = true;
	if (!f->greet_gb)
		f->greet_gb = fy_generic_builder_create(NULL);
	fyai_error_check(s->ctx, f->greet_gb, err,
			 "cannot make the builder of the display frame");
	display = fy_get(s->ctx->cfg->config_doc, "display", fy_invalid);
	frame = fy_mapping(f->greet_gb, "type", "fyai", "subtype", "display",
			   "config", fy_is_mapping(display) ?
			   fy_gb_internalize(f->greet_gb, display) :
			   fy_map_empty);
	rc = frames_send(s, frame);
	fy_generic_builder_reset(f->greet_gb);
	return rc;
err:
	return -1;
}

/* Send the operation @op with @extra, a mapping of its arguments. */
static int frames_op(struct fyai_sink *s, const char *op, fy_generic extra)
{
	struct sink_frames *f = s->state;
	fy_generic frame, key, value;

	if (frames_greet(s))
		return -1;
	frame = fy_mapping(f->gb, "type", "fyai", "subtype", "sink", "op", op);
	fy_foreach_key_value(key, value, extra)
		frame = fy_assoc(f->gb, frame, key, value);
	return frames_put(s, frame);
}

static int frames_doc_begin(struct fyai_sink *s, const struct fyai_sink_doc *doc)
{
	struct sink_frames *f = s->state;

	return frames_op(s, "doc_begin", fy_mapping(f->gb, "kind",
						    frame_kinds[doc->kind]));
}

static int frames_doc_append(struct fyai_sink *s, const char *text, size_t len)
{
	struct sink_frames *f = s->state;

	return frames_op(s, "doc_append", fy_mapping(f->gb, "data",
				fy_gb_string_size_create(f->gb, text, len)));
}

static int frames_doc_end(struct fyai_sink *s, bool aborted)
{
	struct sink_frames *f = s->state;

	return frames_op(s, "doc_end", fy_mapping(f->gb, "aborted", aborted));
}

static void frames_doc_discard(struct fyai_sink *s)
{
	(void)frames_op(s, "doc_discard", fy_map_empty);
}

static int frames_doc_pause(struct fyai_sink *s)
{
	return frames_op(s, "doc_pause", fy_map_empty);
}

static int frames_doc_resume(struct fyai_sink *s)
{
	return frames_op(s, "doc_resume", fy_map_empty);
}

static int frames_markdown(struct fyai_sink *s, enum fyai_sink_stream stream,
			   const char *md)
{
	struct sink_frames *f = s->state;

	return frames_op(s, "markdown", fy_mapping(f->gb,
				"stream", frame_streams[stream], "data", md ? md : ""));
}

static int frames_write(struct fyai_sink *s, enum fyai_sink_stream stream,
			const char *buf, size_t len)
{
	struct sink_frames *f = s->state;

	return frames_op(s, "write", fy_mapping(f->gb,
				"stream", frame_streams[stream],
				"data", fy_gb_string_size_create(f->gb, buf, len)));
}

static void frames_destroy(struct fyai_sink *s)
{
	struct sink_frames *f = s->state;

	fy_generic_builder_destroy(f->greet_gb);
	fy_generic_builder_destroy(f->gb);
	free(f);
}

static const struct fyai_sink_ops sink_frames_ops = {
	.name		= "frames",
	.doc_begin	= frames_doc_begin,
	.doc_append	= frames_doc_append,
	.doc_end	= frames_doc_end,
	.doc_discard	= frames_doc_discard,
	.doc_pause	= frames_doc_pause,
	.doc_resume	= frames_doc_resume,
	.markdown	= frames_markdown,
	.write		= frames_write,
	.destroy	= frames_destroy,
};

struct fyai_sink *fyai_sink_create_frames(struct fyai_ctx *ctx, int fd)
{
	struct fyai_sink *s;
	struct sink_frames *f;

	s = calloc(1, sizeof(*s));
	f = calloc(1, sizeof(*f));
	if (f)
		f->gb = fy_generic_builder_create(NULL);
	if (!s || !f || !f->gb) {
		if (f)
			fy_generic_builder_destroy(f->gb);
		free(f);
		free(s);
		return NULL;
	}
	f->fd = fd;
	s->ctx = ctx;
	s->ops = &sink_frames_ops;
	s->state = f;
	fyai_flow_reset(&s->flow, ctx->cfg);
	return s;
}

/* The stream that @name gives, or -1. */
static int frames_stream(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(frame_streams); i++)
		if (name && !strcmp(name, frame_streams[i]))
			return (int)i;
	return -1;
}

/* Take the display settings of the worker for this run, before any output. */
static int frames_display(struct fyai_ctx *ctx, fy_generic config)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic layer;
	int rc;

	if (!fy_is_mapping(config))
		return 0;
	layer = fy_mapping(cfg->gb, "display", fy_gb_internalize(cfg->gb, config));
	rc = fyai_config_apply(cfg, layer);
	fyai_error_check(ctx, !rc, err,
			 "cannot take the display settings of the server");
	if (cfg->markdown)
		fyai_markdown_load_style(cfg);
	fyai_flow_reset(&ctx->sink->flow, cfg);
	return 0;
err:
	return -1;
}

int fyai_sink_frames_present(struct fyai_ctx *ctx, fy_generic frame)
{
	struct fyai_sink *s = ctx->sink;
	struct fyai_sink_doc doc = { .kind = FYAI_SINK_DOC_ASSISTANT };
	fy_generic data = fy_get(frame, "data", fy_invalid);
	const char *op, *kind, *text;
	size_t len = 0;
	int stream;

	if (fy_equal(fy_get(frame, "subtype", ""), "display"))
		return frames_display(ctx, fy_get(frame, "config", fy_invalid));
	op = fy_get(frame, "op", "");
	stream = frames_stream(fy_get(frame, "stream", (const char *)NULL));
	text = fy_is_string(data) ? fy_castp(&data, "") : NULL;
	if (text)
		len = fy_len(data);
	if (!strcmp(op, "markdown") || !strcmp(op, "write")) {
		fyai_error_check(ctx, stream >= 0 && text, err,
				 "the server sent output without a stream");
		if (!strcmp(op, "markdown"))
			return fyai_sink_markdown(s, stream, text);
		return fyai_sink_write(s, stream, text, len);
	}
	if (!strcmp(op, "doc_begin")) {
		kind = fy_get(frame, "kind", "");
		if (!strcmp(kind, "system"))
			doc.kind = FYAI_SINK_DOC_SYSTEM;
		else if (!strcmp(kind, "user"))
			doc.kind = FYAI_SINK_DOC_USER;
		return fyai_sink_doc_begin(s, doc.kind);
	}
	if (!strcmp(op, "doc_append")) {
		fyai_error_check(ctx, text, err, "the server sent a document without text");
		return fyai_sink_doc_append(s, text, len);
	}
	if (!strcmp(op, "doc_end"))
		return fyai_sink_doc_end(s, fy_get(frame, "aborted", false));
	if (!strcmp(op, "doc_discard")) {
		fyai_sink_doc_discard(s);
		return 0;
	}
	if (!strcmp(op, "doc_pause"))
		return fyai_sink_doc_pause(s);
	if (!strcmp(op, "doc_resume"))
		return fyai_sink_doc_resume(s);
	fyai_error(ctx, "the server sent the unknown output operation '%s'", op);
err:
	return -1;
}
