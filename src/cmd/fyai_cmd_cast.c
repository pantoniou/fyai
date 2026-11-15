/*
 * fyai_cmd_cast.c - play and list a recording of a session
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A recording is the file that --cast writes (src/front/fyai_cast.c): a header line
 * and one array for each event, [seconds, kind, text].
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include <unistd.h>

#include "fyai_cast_file.h"
#include "fyai_cast_player.h"
#include "fyai_cmd_int.h"
#include "fyai_event.h"
#include "fyai_sink.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* What leaves a terminal as the session did: the cursor, the screens and the mouse. */
#define CAST_RESET "\x1b[?2026l\x1b[?1049l\x1b[?1000l\x1b[?1002l\x1b[?1003l" \
		   "\x1b[?1006l\x1b[?2004l\x1b[?25h\x1b[0m"

/* Wait @ms on the loop of the invocation; false after an interrupt. */
static bool cast_wait(struct fyai_ctx *ctx, double ms)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	double left = ms;

	while (left > 0) {
		if (fyai_interrupt_pending(ctx)) {
			fyai_event_interrupt_ack(ctx->loop);
			return false;
		}
		if (fyai_event_loop_step(el, left > 50 ? 50 : (int)ceil(left)) < 0)
			return false;
		left -= 50;
	}
	return !fyai_interrupt_pending(ctx);
}

int fyai_cmd_cast_play(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	double speed = fy_get(call->args, "speed", 1.0);
	double idle = fy_get(call->args, "idle_limit", 2.0);
	const char *path = fyai_cmd_arg_str(call, "file");
	struct fyai_cast_file f;
	fy_generic header, ev, data;
	double t, prev = 0, gap;
	const char *text;
	bool stopped = false;
	int rc = -1;

	(void)result;
	/* A person at a terminal gets the player; a pipe or --plain gets the bytes. */
	if (!fyai_cmd_arg_bool(call, "plain") && isatty(STDIN_FILENO) &&
	    isatty(STDOUT_FILENO))
		return fyai_cast_play_interactive(ctx, path, speed);
	if (fyai_cast_file_open(fyai_cmd_call_ctx(call), &f, path, &header))
		goto out;
	if (speed <= 0)
		speed = 1.0;
	while (fy_is_valid(ev = fyai_cast_file_next(&f))) {
		t = fy_number(fy_get_at(ev, 0), 0.0);
		if (!fy_equal(fy_get_at(ev, 1), "o")) {
			continue;
		}
		gap = t - prev;
		prev = t;
		/* A long pause in the recording is shortened to the limit. */
		if (idle > 0 && gap > idle)
			gap = idle;
		if (gap > 0 && !cast_wait(ctx, gap * 1000.0 / speed)) {
			stopped = true;
			break;
		}
		data = fy_get_at(ev, 2);
		text = fy_castp(&data, "");
		(void)fyai_sink_write(ctx->out->sink, FYAI_SINK_MACHINE, text, strlen(text));
	}
	/* What the session left on: put the terminal back. */
	(void)fyai_sink_write(ctx->out->sink, FYAI_SINK_MACHINE, CAST_RESET,
			      sizeof(CAST_RESET) - 1);
	(void)stopped;
	rc = 0;
out:
	fyai_cast_file_close(&f);
	return rc;
}

int fyai_cmd_cast_markers(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *path = fyai_cmd_arg_str(call, "file");
	struct fyai_cast_file f;
	fy_generic header, ev, data, rows;
	int rc = -1;

	if (fyai_cast_file_open(fyai_cmd_call_ctx(call), &f, path, &header))
		goto out;
	rows = fy_sequence(call->gb);
	while (fy_is_valid(ev = fyai_cast_file_next(&f))) {
		if (!fy_equal(fy_get_at(ev, 1), "m"))
			continue;
		data = fy_get_at(ev, 2);
		rows = fy_append(call->gb, rows, fy_mapping(call->gb,
			"time", fy_number(fy_get_at(ev, 0), 0.0),
			"marker", fy_value(call->gb, fy_castp(&data, ""))));
	}
	*result = rows;
	rc = 0;
out:
	fyai_cast_file_close(&f);
	return rc;
}

int fyai_cmd_cast_info(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *path = fyai_cmd_arg_str(call, "file");
	struct fyai_cast_file f;
	fy_generic header, info, key, value;
	int rc = -1;

	if (fyai_cast_file_open(fyai_cmd_call_ctx(call), &f, path, &header))
		goto out;
	info = fy_mapping(call->gb,
		"width", fy_get(header, "width", 0LL),
		"height", fy_get(header, "height", 0LL),
		"timestamp", fy_get(header, "timestamp", 0LL));
	if (fy_is_valid(fy_get(header, "command", fy_invalid)))
		info = fy_assoc(call->gb, info, "command",
				fy_gb_internalize(call->gb, fy_get(header, "command")));
	/* What the program knew: one row for each, since a row holds no mapping. */
	fy_foreach_key_value(key, value, fy_get(header, "fyai", fy_invalid))
		info = fy_assoc(call->gb, info,
				fy_stringf(call->gb, "fyai %s", fy_castp(&key, "")),
				fy_gb_internalize(call->gb, value));
	*result = info;
	rc = 0;
out:
	fyai_cast_file_close(&f);
	return rc;
}
