/*
 * fyai_cmd_media.c - handlers of the image commands
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A command reports what it attached and returns data; the caller presents
 * the result. The attachment itself waits in the context, and the next user
 * message takes it (fyai_make_user_message).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_config.h"
#include "fyai_media.h"

#define FYAI_MODULE FYAIEM_MEDIA

/*
 * The result of an attachment: what the image is, and whether it waits. The
 * pending part is stated because a command that attaches during a turn leaves
 * the image for the message after it.
 */
static fy_generic media_add_result(struct fyai_cmd_call *call,
				   fy_generic pending)
{
	struct fyai_ctx *ctx = call->ctx;
	fy_generic part, out;

	if (!fy_is_sequence(pending) || fy_empty(pending))
		return fy_invalid;
	part = fy_get_at(pending, fy_len(pending) - 1);

	out = fy_mapping(call->gb,
		"media_type", fy_get(part, "media_type", ""),
		"width", fy_get(part, "width", 0LL),
		"height", fy_get(part, "height", 0LL),
		"bytes", fy_get(part, "bytes", 0LL),
		"pending", true);
	if (fy_is_valid(fy_get(part, "name", fy_invalid)))
		out = fy_assoc(call->gb, out, "name", fy_get(part, "name"));
	/*
	 * An attachment is only a part of a request that this grammar carries
	 * and a model that reads images: say so here, where the user is
	 * looking, and not only when the request fails.
	 */
	if (!ctx->cfg->image_input_supported)
		out = fy_assoc(call->gb, out, "note",
			fy_sprintfa("the model '%s' takes no image input; the "
				    "next message cannot be sent",
				    ctx->cfg->model ? ctx->cfg->model : ""));
	return out;
}

int fyai_cmd_image_add(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	const char *path = fyai_cmd_arg_str(call, "path");
	const char *type = fyai_cmd_arg_str(call, "media_type");
	const char *name = fyai_cmd_arg_str(call, "name");

	if (fyai_media_attach_path(ctx, path, type, name))
		return -1;
	*result = media_add_result(call, ctx->media_pending);
	return 0;
}

int fyai_cmd_image_paste(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	const char *name = fyai_cmd_arg_str(call, "name");
	fy_generic pending, last;

	if (fyai_media_attach_clipboard(ctx))
		return -1;
	pending = ctx->media_pending;
	/* The helper publishes under "clipboard"; a stated name replaces it. */
	if (name && *name && fy_is_sequence(pending) && !fy_empty(pending)) {
		last = fy_get_at(pending, fy_len(pending) - 1);
		last = fy_assoc(ctx->arena->gb, last, "name", name);
		pending = fy_replace(ctx->arena->gb, pending, fy_len(pending) - 1, last);
		ctx->media_pending = pending;
	}
	*result = media_add_result(call, pending);
	return 0;
}

/*
 * Attach a file by what it holds: an image as with image add, or a text file
 * that the model reads inline. The result says which.
 */
int fyai_cmd_attach(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	const char *path = fyai_cmd_arg_str(call, "path");
	fy_generic part = fy_invalid;
	char size[48];

	if (fyai_media_attach_file(ctx, path, false, &part))
		return -1;
	if (!fyai_media_part_is_file(part)) {
		*result = media_add_result(call, ctx->media_pending);
		return 0;
	}
	fyai_media_file_size(size, sizeof(size), part);
	*result = fy_mapping(call->gb,
		"file", fy_get(part, "file", ""),
		"size", fy_value(call->gb, size),
		"lines", fy_get(part, "lines", 0LL),
		"bytes", fy_get(part, "bytes", 0LL),
		"pending", true);
	return 0;
}

int fyai_cmd_image_clear(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	size_t count;

	count = fy_len(ctx->media_pending);
	fyai_media_pending_clear(ctx);
	*result = fy_mapping(call->gb, "count", (long long)count);
	return 0;
}

int fyai_cmd_image_list(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	fy_generic out, part;
	long long width, height;

	out = fy_seq_empty;
	fy_foreach(part, ctx->media_pending) {
		/* a text file: its lines in place of a pixel size */
		if (fyai_media_part_is_file(part)) {
			out = fy_append(call->gb, out, fy_mapping(call->gb,
				"media_type", "text/plain",
				"size", fy_sprintfa("%lld lines",
						    fy_get(part, "lines", 0LL)),
				"bytes", fy_get(part, "bytes", 0LL),
				"name", fy_get(part, "file", "")));
			continue;
		}
		width = fy_get(part, "width", 0LL);
		height = fy_get(part, "height", 0LL);
		out = fy_append(call->gb, out, fy_mapping(call->gb,
			"media_type", fy_get(part, "media_type", ""),
			"size", fy_sprintfa("%lldx%lld", width, height),
			"bytes", fy_get(part, "bytes", 0LL),
			"name", fy_get(part, "name", "")));
	}
	*result = out;
	return 0;
}
