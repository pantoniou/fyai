/*
 * fyai_cmd_page.c - handlers of the page commands
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
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
#include "fyai_page.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_workpane.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_DISPLAY

int fyai_cmd_page(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_ui_page_report(call->ctx);
}

int fyai_cmd_page_review(struct fyai_cmd_call *call, fy_generic *result)
{
	bool on;

	if (fyai_ui_page_review(call->ctx, fyai_cmd_arg_str(call, "how"), &on))
		return -1;
	*result = fy_mapping(call->gb, "review", on ? "on" : "off");
	return 0;
}

/* The state of the sample @name, before the fit. */
static void page_sample_state(const char *name, int height,
			      struct fyai_page_state *st)
{
	static const struct fyai_page_ask_option options[] = {
		{ "Yes", "Apply it as written", false },
		{ "No", "Leave the files alone", false },
		{ "Show me the diff first", "Open the change before deciding", false },
	};
	static const struct fyai_page_ask ask = {
		.header = "Patch",
		.question = "Apply the patch?",
		.from = "main/agent:review",
		.options = options,
		.noptions = sizeof(options) / sizeof(options[0]),
		.count = 1,
		.waiting = 1,
	};

	memset(st, 0, sizeof(*st));
	st->header = "~/project  main  gpt-5";
	st->elapsed = " 12s";
	st->activity = "*";
	st->gutter_cols = 2;
	st->status = "working: 2 tools";
	st->hint = "Ctrl-T moves the keys to a tile";
	st->prompt_rows = 1;
	st->prompt_card = true;
	st->input_mode = "prompt";
	st->fullscreen = strcmp(name, "inline") != 0;
	/* The pane and the tail ask for rows; the fit gives them theirs. */
	st->pane_rows = 6;
	st->cap = "3 tiles <fy-fill/> Ctrl-T";
	st->tail_rows = height;
	if (!strcmp(name, "question")) {
		st->input_mode = "ask";
		st->ask = &ask;
	} else if (!strcmp(name, "popup")) {
		st->popup_title = "/stats";
	}
}

/* The tiles of a sample: three screens, in a row of the band or in the side
 * column of @lay, at the rows the fit gave them. */
#define PAGE_SAMPLE_TILES 3
static int page_sample_tiles(struct fyai_ctx *ctx,
			     const struct fyai_page_layout *lay,
			     struct fyai_page_state *st,
			     struct response_buffer *grid)
{
	struct fyai_page_cell cells[PAGE_SAMPLE_TILES];
	struct fyai_workpane_grid g;
	int i, height;

	memset(&g, 0, sizeof(g));
	memset(cells, 0, sizeof(cells));
	height = lay->side ? st->transcript_rows : st->pane_rows;
	if (height < 1)
		return 0;
	if (lay->side) {
		fyai_page_side_place(lay, PAGE_SAMPLE_TILES, false, &g);
	} else {
		g.rows = 1;
		g.cols = PAGE_SAMPLE_TILES;
		for (i = 0; i < PAGE_SAMPLE_TILES; i++) {
			g.place[i].col = i;
			g.place[i].row_span = g.place[i].col_span = 1;
		}
	}
	for (i = 0; i < PAGE_SAMPLE_TILES; i++) {
		cells[i].slot = (unsigned int)i + 1;
		cells[i].row = g.place[i].row;
		cells[i].col = g.place[i].col;
		cells[i].row_span = cells[i].col_span = 1;
		cells[i].head_rows = 1;
		cells[i].rows = 12;
		cells[i].screen = true;
	}
	if (fyai_page_grid_lead(ctx, &g, cells, PAGE_SAMPLE_TILES, height, "",
				0, lay->side ? "transcript" : NULL, grid, NULL))
		return -1;
	st->pane_source = grid->data;
	return 0;
}

int fyai_cmd_page_review_sample(struct fyai_cmd_call *call,
				fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct response_buffer picture = {0}, grid = {0};
	struct fyai_page_layout lay;
	const struct fyai_page_action *actions;
	struct fyai_page_state st;
	struct fyai_page *pg = NULL;
	size_t nactions;
	int cols, rows, rc;

	cols = (int)fy_get(call->args, "width", 80LL);
	rows = (int)fy_get(call->args, "height", 24LL);
	page_sample_state(fyai_cmd_arg_str(call, "sample"), rows, &st);
	fyai_ui_page_actions(&actions, &nactions);
	st.actions = actions;
	st.nactions = nactions;
	pg = fyai_page_create_from(ctx, fyai_cmd_arg_str(call, "page"), true,
				   actions, nactions);
	fyai_error_check(ctx, pg, err_out, "cannot make the page");
	rc = fyai_page_layout(pg, cols, rows, st.fullscreen, &lay);
	fyai_error_check(ctx, !rc, err_out,
			 "the page document has a layout that is not valid");
	st.layout = lay.name;
	if (lay.side && !st.popup_title) {
		st.pane_side = true;
		st.pane_rows = 0;
	}
	fyai_page_fit(&st, rows);
	rc = page_sample_tiles(ctx, &lay, &st, &grid);
	fyai_error_check(ctx, !rc, err_out, "cannot build the sample tiles");
	rc = fyai_page_review(pg, fyai_page_state_generic(call->gb, &st), cols,
			      rows, call->gb, &picture, result);
	if (rc)
		goto err_out;
	/* The picture is presentation; a machine format takes the areas. */
	if (call->format == FYAI_CMD_OUT_MARKDOWN && picture.data) {
		rc = fyai_sink_write(ctx->sink, FYAI_SINK_NOTICE, picture.data,
				     picture.len);
		fyai_error_check(ctx, !rc, err_out,
				 "cannot write the picture of the page");
	}
	free(picture.data);
	free(grid.data);
	fyai_page_destroy(pg);
	return 0;

err_out:
	free(picture.data);
	free(grid.data);
	fyai_page_destroy(pg);
	return -1;
}
