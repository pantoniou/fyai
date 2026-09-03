/*
 * fyai_workpane_test.c - the behavioral contract of the work-pane manager
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * The manager decides geometry, focus, and zoom. What it decides is state of
 * its own, so these tests run without a display: a surface is an identity
 * here, and the library that draws one proves its own drawing elsewhere.
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <string.h>

#include "fyai.h"
#include "fyai_test.h"
#include "fyai_workpane.h"
#include "fyai_config.h"
#include <libfytimui.h>

#include "fyai_ui.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(workpane, focus_keeps_layout, workpane_focus_keeps_layout)
FYAI_TEST_ENTRY(workpane, keeps_preference, workpane_content_keeps_preference)
FYAI_TEST_ENTRY(workpane, grant_not_preference, workpane_grant_is_not_preference)
FYAI_TEST_ENTRY(workpane, resize_recalculates, workpane_resize_recalculates)
FYAI_TEST_ENTRY(workpane, zoom_keeps_focus, workpane_zoom_keeps_focus)
FYAI_TEST_ENTRY(workpane, retirement_is_atomic, workpane_retirement_is_atomic)
FYAI_TEST_ENTRY(workpane, full_is_active, workpane_full_is_active)
FYAI_TEST_ENTRY(workpane, layout_main_top, workpane_layout_main_top)
FYAI_TEST_ENTRY(workpane, layout_main_left, workpane_layout_main_left)
FYAI_TEST_ENTRY(workpane, layout_stack_and_columns, workpane_layout_stack_cols)
FYAI_TEST_ENTRY(workpane, custom_policy_places, workpane_custom_policy_places)
FYAI_TEST_ENTRY(workpane, ladder_collapses, workpane_ladder_collapses)
FYAI_TEST_ENTRY(workpane, notices_take_a_row, workpane_notices_take_a_row)
FYAI_TEST_ENTRY(workpane, position_is_config, workpane_position_is_config)
FYAI_TEST_ENTRY(workpane, focus_colour, workpane_focus_colour)
FYAI_TEST_ENTRY(workpane, grid_has_no_holes, workpane_grid_has_no_holes)
FYAI_TEST_ENTRY(workpane, tiles_are_placed_in_age, workpane_tiles_in_age)
FYAI_TEST_ENTRY(workpane, keys_reach_the_program, workpane_keys_reach_program)

static struct fyai_cfg wpt_cfg;
static struct fyai_ctx wpt_ctx = { .cfg = &wpt_cfg };

/* Grants the tiles were handed, so a test can prove one was applied. */
static int wpt_shell_rows, wpt_agent_rows;

static void wpt_shell_grant(void *owner, int rows, int cols)
{
	(void)owner;
	(void)cols;
	wpt_shell_rows = rows;
}

static void wpt_agent_grant(void *owner, int rows, int cols)
{
	(void)owner;
	(void)cols;
	wpt_agent_rows = rows;
}

static const struct fyai_workpane_tile_ops wpt_shell_ops = {
	.apply_grant = wpt_shell_grant,
};

static const struct fyai_workpane_tile_ops wpt_agent_ops = {
	.apply_grant = wpt_agent_grant,
};

/*
 * A surface is compared by identity and never dereferenced without a display,
 * so these addresses are the two tiles of every test below.
 */
static int wpt_shell_id, wpt_agent_id;

#define WPT_SHELL ((struct fytim_surface *)&wpt_shell_id)
#define WPT_AGENT ((struct fytim_surface *)&wpt_agent_id)

/* A manager on a 40-row terminal with the named pane height policy. */
static struct fyai_workpane_manager *wpt_open(const char *policy, int fixed)
{
	struct fyai_workpane_manager *wm;

	memset(&wpt_cfg, 0, sizeof(wpt_cfg));
	wpt_cfg.work_zoom_rows = policy;
	wpt_cfg.work_zoom_fixed_rows = fixed;
	wpt_ctx.workpane = NULL;
	wm = fyai_workpane_create(&wpt_ctx, NULL);
	FYAI_TCHECK(wm != NULL);
	wpt_ctx.workpane = wm;
	fyai_workpane_terminal_resize(wm, 40, 100);
	return wm;
}

static void wpt_close(struct fyai_workpane_manager *wm)
{
	fyai_workpane_destroy(wm);
	wpt_ctx.workpane = NULL;
}

/* A shell that fills the pane and a sub-agent that keeps its own height. */
static void wpt_register_pair(struct fyai_workpane_manager *wm)
{
	FYAI_TCHECK(!fyai_workpane_register(wm, WPT_SHELL,
					    FYAI_WORKPANE_TILE_SHELL,
					    &wpt_shell_id, &wpt_shell_ops,
					    FYAI_WORKPANE_FILL, 0));
	FYAI_TCHECK(!fyai_workpane_register(wm, WPT_AGENT,
					    FYAI_WORKPANE_TILE_AGENT,
					    &wpt_agent_id, &wpt_agent_ops,
					    40, 0));
}

int workpane_focus_keeps_layout(void)
{
	struct fyai_workpane_manager *wm = wpt_open("half", 0);
	int cap, shell, agent;

	wpt_register_pair(wm);
	fyai_workpane_reconcile(wm);
	cap = fyai_workpane_resolved_rows(wm);
	shell = fyai_workpane_tile_preferred_rows(wm, WPT_SHELL);
	agent = fyai_workpane_tile_preferred_rows(wm, WPT_AGENT);

	/* Focus is keyboard ownership. It states nothing about geometry. */
	fyai_workpane_set_focus(wm, WPT_SHELL);
	fyai_workpane_set_focus(wm, WPT_AGENT);
	fyai_workpane_clear_focus(wm);
	fyai_workpane_set_focus(wm, WPT_SHELL);
	fyai_workpane_reconcile(wm);

	FYAI_TCHECK(fyai_workpane_resolved_rows(wm) == cap);
	FYAI_TCHECK(fyai_workpane_tile_preferred_rows(wm, WPT_SHELL) == shell);
	FYAI_TCHECK(fyai_workpane_tile_preferred_rows(wm, WPT_AGENT) == agent);
	FYAI_TCHECK(fyai_workpane_focused(wm) == WPT_SHELL);

	wpt_close(wm);
	printf("ok - moving focus changes no pane or tile geometry\n");
	return 0;
}

int workpane_content_keeps_preference(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);

	wpt_register_pair(wm);
	/* The sub-agent has drawn two rows and asks for forty regardless. */
	fyai_workpane_grid_resized(wm, WPT_AGENT, 2, 100);
	fyai_workpane_set_focus(wm, WPT_AGENT);
	fyai_workpane_reconcile(wm);

	FYAI_TCHECK(fyai_workpane_tile_preferred_rows(wm, WPT_AGENT) == 40);

	wpt_close(wm);
	printf("ok - what a tile has drawn is not its preferred height\n");
	return 0;
}

int workpane_grant_is_not_preference(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);

	wpt_register_pair(wm);
	fyai_workpane_reconcile(wm);

	/* A twelve-row grant reaches the program and stops there. */
	fyai_workpane_grid_resized(wm, WPT_AGENT, 12, 100);
	fyai_workpane_reconcile(wm);
	FYAI_TCHECK(fyai_workpane_tile_preferred_rows(wm, WPT_AGENT) == 40);

	wpt_close(wm);
	printf("ok - a grant does not become the next request\n");
	return 0;
}

int workpane_resize_recalculates(void)
{
	struct fyai_workpane_manager *wm = wpt_open("half", 0);

	wpt_register_pair(wm);
	fyai_workpane_reconcile(wm);
	FYAI_TCHECK(fyai_workpane_resolved_rows(wm) == 20);

	fyai_workpane_terminal_resize(wm, 60, 100);
	FYAI_TCHECK(fyai_workpane_resolved_rows(wm) == 30);

	/* A quarter of the same terminal, and then a direct row count. */
	fyai_workpane_set_disposition(wm, FYAI_WORKPANE_QUARTER, 0);
	FYAI_TCHECK(fyai_workpane_resolved_rows(wm) == 15);
	fyai_workpane_set_disposition(wm, FYAI_WORKPANE_FIXED, 12);
	FYAI_TCHECK(fyai_workpane_resolved_rows(wm) == 12);

	wpt_close(wm);
	printf("ok - a fraction follows the terminal it is a fraction of\n");
	return 0;
}

int workpane_zoom_keeps_focus(void)
{
	struct fyai_workpane_manager *wm = wpt_open("half", 0);
	int cap;

	wpt_register_pair(wm);
	fyai_workpane_reconcile(wm);
	cap = fyai_workpane_resolved_rows(wm);

	fyai_workpane_set_focus(wm, WPT_SHELL);
	FYAI_TCHECK(!fyai_workpane_set_zoom(wm, WPT_AGENT));
	FYAI_TCHECK(fyai_workpane_focused(wm) == WPT_SHELL);
	FYAI_TCHECK(fyai_workpane_zoomed(wm) == WPT_AGENT);

	fyai_workpane_clear_zoom(wm);
	FYAI_TCHECK(fyai_workpane_focused(wm) == WPT_SHELL);
	FYAI_TCHECK(fyai_workpane_zoomed(wm) == NULL);
	FYAI_TCHECK(fyai_workpane_resolved_rows(wm) == cap);
	FYAI_TCHECK(fyai_workpane_disposition(wm) == FYAI_WORKPANE_HALF);

	wpt_close(wm);
	printf("ok - zoom selects a tile and leaves focus where it was\n");
	return 0;
}

int workpane_retirement_is_atomic(void)
{
	struct fyai_workpane_manager *wm = wpt_open("half", 0);
	int cap;

	wpt_register_pair(wm);
	fyai_workpane_reconcile(wm);
	cap = fyai_workpane_resolved_rows(wm);

	fyai_workpane_set_focus(wm, WPT_AGENT);
	FYAI_TCHECK(!fyai_workpane_set_zoom(wm, WPT_AGENT));

	/* The tile that held both goes, and neither is left behind. */
	fyai_workpane_unregister(wm, WPT_AGENT);
	FYAI_TCHECK(fyai_workpane_focused(wm) == NULL);
	FYAI_TCHECK(fyai_workpane_zoomed(wm) == NULL);
	FYAI_TCHECK(fyai_workpane_tile_owner(wm, WPT_AGENT, NULL) == NULL);
	FYAI_TCHECK(fyai_workpane_resolved_rows(wm) == cap);

	/* The tile that remains keeps the disposition and can take focus. */
	FYAI_TCHECK(fyai_workpane_focus_next(wm));
	FYAI_TCHECK(fyai_workpane_focused(wm) == WPT_SHELL);
	FYAI_TCHECK(fyai_workpane_tile_preferred_rows(wm, WPT_SHELL) ==
		    FYAI_WORKPANE_FILL);

	wpt_close(wm);
	printf("ok - retiring a tile leaves no stale focus, zoom, or cap\n");
	return 0;
}

int workpane_full_is_active(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);

	/* FULL is a disposition the manager holds, not an absent one. */
	FYAI_TCHECK(fyai_workpane_disposition(wm) == FYAI_WORKPANE_FULL);
	fyai_workpane_reconcile(wm);
	FYAI_TCHECK(fyai_workpane_resolved_rows(wm) == 0);

	/* Cycling passes through half and quarter and returns to full. */
	fyai_workpane_cycle_disposition(wm);
	FYAI_TCHECK(fyai_workpane_disposition(wm) == FYAI_WORKPANE_HALF);
	fyai_workpane_cycle_disposition(wm);
	FYAI_TCHECK(fyai_workpane_disposition(wm) == FYAI_WORKPANE_QUARTER);
	fyai_workpane_cycle_disposition(wm);
	FYAI_TCHECK(fyai_workpane_disposition(wm) == FYAI_WORKPANE_FULL);

	/* A configured pane ceiling is a further limit on any disposition. */
	wpt_cfg.work_max_rows = 8;
	fyai_workpane_adopt_config(wm);
	fyai_workpane_reconcile(wm);
	FYAI_TCHECK(fyai_workpane_resolved_rows(wm) == 8);

	wpt_close(wm);
	printf("ok - full is an active disposition, not an unset one\n");
	return 0;
}

/*
 * The arrangements. A policy is a decision about placement, so what is
 * asserted here is the grid it produced and where each tile went - not what
 * the layout library then drew, which the library proves in its own suite.
 */
static struct fyai_workpane_grid wpt_grid;
static int wpt_grid_n;

static int wpt_capture(void *user, const struct fyai_workpane_tile_info *tiles,
		       int n, int rows, int cols,
		       struct fyai_workpane_grid *out)
{
	(void)user;
	(void)tiles;
	(void)rows;
	(void)cols;
	/* A policy of our own: one column, newest at the top. */
	memset(out, 0, sizeof(*out));
	out->rows = n;
	out->cols = 1;
	{
		int i;

		for (i = 0; i < n; i++) {
			out->place[i].row = n - 1 - i;
			out->place[i].col = 0;
			out->place[i].row_span = 1;
			out->place[i].col_span = 1;
		}
	}
	wpt_grid = *out;
	wpt_grid_n = n;
	return 0;
}

/* Place @n tiles under the current layout and report the grid it chose. */
static void wpt_place(struct fyai_workpane_manager *wm, int n)
{
	struct fyai_workpane_tile_info info[FYAI_WORKPANE_TILES_MAX];
	int i;

	for (i = 0; i < n; i++) {
		info[i].kind = FYAI_WORKPANE_TILE_SHELL;
		info[i].preferred_rows = 10;
		info[i].focused = false;
	}
	wpt_grid_n = n;
	FYAI_TCHECK(!fyai_workpane_place(wm, info, n, &wpt_grid));
}

int workpane_layout_main_top(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);

	fyai_workpane_set_layout(wm, FYAI_WORKPANE_LAYOUT_MAIN_TOP, 0);
	wpt_place(wm, 3);
	/* Two rows of two columns: the first tile spans the top. */
	FYAI_TCHECK(wpt_grid.rows == 2 && wpt_grid.cols == 2);
	FYAI_TCHECK(wpt_grid.place[0].row == 0 && wpt_grid.place[0].col == 0);
	FYAI_TCHECK(wpt_grid.place[0].col_span == 2);
	FYAI_TCHECK(wpt_grid.place[1].row == 1 && wpt_grid.place[1].col == 0);
	FYAI_TCHECK(wpt_grid.place[2].row == 1 && wpt_grid.place[2].col == 1);

	wpt_close(wm);
	printf("ok - main-top gives the oldest screen the whole top row\n");
	return 0;
}

int workpane_layout_main_left(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);

	fyai_workpane_set_layout(wm, FYAI_WORKPANE_LAYOUT_MAIN_LEFT, 0);
	wpt_place(wm, 3);
	/* Two rows of two columns: the first tile spans the left column. */
	FYAI_TCHECK(wpt_grid.rows == 2 && wpt_grid.cols == 2);
	FYAI_TCHECK(wpt_grid.place[0].col == 0 && wpt_grid.place[0].row_span == 2);
	FYAI_TCHECK(wpt_grid.place[1].col == 1 && wpt_grid.place[1].row == 0);
	FYAI_TCHECK(wpt_grid.place[2].col == 1 && wpt_grid.place[2].row == 1);

	wpt_close(wm);
	printf("ok - main-left gives the oldest screen a column of its own\n");
	return 0;
}

int workpane_layout_stack_cols(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);

	fyai_workpane_set_layout(wm, FYAI_WORKPANE_LAYOUT_STACK, 0);
	wpt_place(wm, 3);
	FYAI_TCHECK(wpt_grid.rows == 3 && wpt_grid.cols == 1);
	FYAI_TCHECK(wpt_grid.place[2].row == 2);

	fyai_workpane_set_layout(wm, FYAI_WORKPANE_LAYOUT_COLUMNS, 2);
	wpt_place(wm, 3);
	/* Three tiles over two columns wrap into a second row. */
	FYAI_TCHECK(wpt_grid.rows == 2 && wpt_grid.cols == 2);
	FYAI_TCHECK(wpt_grid.place[2].row == 1 && wpt_grid.place[2].col == 0);

	wpt_close(wm);
	printf("ok - stack and columns place every tile\n");
	return 0;
}

int workpane_custom_policy_places(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);

	fyai_workpane_set_policy(wm, wpt_capture, NULL);
	fyai_workpane_set_layout(wm, FYAI_WORKPANE_LAYOUT_CUSTOM, 0);
	wpt_place(wm, 3);
	/* The policy of the host placed them, newest at the top. */
	FYAI_TCHECK(wpt_grid.rows == 3 && wpt_grid.cols == 1);
	FYAI_TCHECK(wpt_grid.place[0].row == 2);
	FYAI_TCHECK(wpt_grid.place[2].row == 0);

	/* Selecting a custom layout with no policy falls back to the pane. */
	fyai_workpane_set_policy(wm, NULL, NULL);
	FYAI_TCHECK(fyai_workpane_place(wm, NULL, 0, &wpt_grid) != 0);

	wpt_close(wm);
	printf("ok - a policy of the host places the tiles\n");
	return 0;
}

int workpane_ladder_collapses(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);
	struct fyai_workpane_ladder ladder = {
		.full_rows = 4, .full_cols = 30, .output_rows = 2,
	};

	wpt_register_pair(wm);
	fyai_workpane_tile_set_ladder(wm, WPT_AGENT, &ladder);

	/* Eighty by eight is a screen anyone can read. */
	FYAI_TCHECK(fyai_workpane_present_at(wm, WPT_AGENT, 8, 80) ==
		    FYAI_WORKPANE_PRESENT_FULL);
	/* Three rows is too short for the screen but holds its output. */
	FYAI_TCHECK(fyai_workpane_present_at(wm, WPT_AGENT, 3, 80) ==
		    FYAI_WORKPANE_PRESENT_OUTPUT);
	/* One row by twenty columns leaves only the head. */
	FYAI_TCHECK(fyai_workpane_present_at(wm, WPT_AGENT, 1, 20) ==
		    FYAI_WORKPANE_PRESENT_HEAD);
	/* A tile granted nothing draws nothing. */
	FYAI_TCHECK(fyai_workpane_present_at(wm, WPT_AGENT, 0, 0) ==
		    FYAI_WORKPANE_PRESENT_HIDDEN);
	/* Narrow but tall is still too narrow to read. */
	FYAI_TCHECK(fyai_workpane_present_at(wm, WPT_AGENT, 20, 20) ==
		    FYAI_WORKPANE_PRESENT_OUTPUT);
	/* A tile with no ladder always draws its program. */
	FYAI_TCHECK(fyai_workpane_present_at(wm, WPT_SHELL, 1, 10) ==
		    FYAI_WORKPANE_PRESENT_FULL);

	wpt_close(wm);
	printf("ok - a tile draws as much as the size it was given allows\n");
	return 0;
}

int workpane_notices_take_a_row(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);
	struct fyai_workpane_tile_info info[3];
	int i;

	for (i = 0; i < 3; i++) {
		info[i].kind = FYAI_WORKPANE_TILE_SHELL;
		info[i].preferred_rows = 10;
		info[i].focused = false;
	}
	/* Two screens and a report of the kind a verb leaves behind. */
	info[2].kind = FYAI_WORKPANE_TILE_NOTICE;

	fyai_workpane_set_layout(wm, FYAI_WORKPANE_LAYOUT_COLUMNS, 2);
	FYAI_TCHECK(!fyai_workpane_place(wm, info, 3, &wpt_grid));

	/* The screens share the first row; the report has the next, whole. */
	FYAI_TCHECK(wpt_grid.rows == 2 && wpt_grid.cols == 2);
	FYAI_TCHECK(wpt_grid.place[0].row == 0 && wpt_grid.place[0].col == 0);
	FYAI_TCHECK(wpt_grid.place[1].row == 0 && wpt_grid.place[1].col == 1);
	FYAI_TCHECK(wpt_grid.place[2].row == 1 && wpt_grid.place[2].col == 0);
	FYAI_TCHECK(wpt_grid.place[2].col_span == 2);
	/* And it takes the rows it needs rather than a share of the pane. */
	FYAI_TCHECK(wpt_grid.row_size[1] == FYAI_WORKPANE_TRACK_FIT);
	FYAI_TCHECK(wpt_grid.row_size[0] == 0);

	/* A report with no work above it still takes a row of its own. */
	info[0].kind = FYAI_WORKPANE_TILE_NOTICE;
	FYAI_TCHECK(!fyai_workpane_place(wm, info, 1, &wpt_grid));
	FYAI_TCHECK(wpt_grid.rows == 1 && wpt_grid.cols == 1);
	FYAI_TCHECK(wpt_grid.row_size[0] == FYAI_WORKPANE_TRACK_FIT);

	wpt_close(wm);
	printf("ok - a report takes a row of its own under the work\n");
	return 0;
}

/*
 * Where the pane stands is configuration the manager holds, and it is
 * independent of the arrangement inside the pane: a pane below the prompt
 * still tiles its screens the way the layout says.
 */
int workpane_position_is_config(void)
{
	struct fyai_workpane_manager *wm;
	struct fyai_workpane_grid grid;

	/* The default keeps the pane above the prompt. */
	wm = wpt_open("full", 0);
	FYAI_TCHECK(fyai_workpane_position(wm) == FYAI_WORKPANE_POS_ABOVE);
	wpt_close(wm);

	/* The configured word selects the rows below it. */
	wm = wpt_open("full", 0);
	wpt_cfg.work_position = "below-prompt";
	fyai_workpane_adopt_config(wm);
	FYAI_TCHECK(fyai_workpane_position(wm) == FYAI_WORKPANE_POS_BELOW);

	/* An unknown word is the default, not a third position. */
	wpt_cfg.work_position = "beside";
	fyai_workpane_adopt_config(wm);
	FYAI_TCHECK(fyai_workpane_position(wm) == FYAI_WORKPANE_POS_ABOVE);

	/* The position does not disturb the arrangement. */
	fyai_workpane_set_position(wm, FYAI_WORKPANE_POS_BELOW);
	FYAI_TCHECK(fyai_workpane_position(wm) == FYAI_WORKPANE_POS_BELOW);
	wpt_register_pair(wm);
	fyai_workpane_set_layout(wm, FYAI_WORKPANE_LAYOUT_STACK, 0);
	FYAI_TCHECK(!fyai_workpane_place(wm, NULL, 0, &grid));
	FYAI_TCHECK(grid.rows == 2 && grid.cols == 1);
	FYAI_TCHECK(fyai_workpane_position(wm) == FYAI_WORKPANE_POS_BELOW);
	wpt_close(wm);
	return 0;
}

/*
 * The ground of a focused tile is a colour the user gave. A value that is
 * not one leaves the focus mark as it was: a tile grounded in black would
 * say the setting worked.
 */
int workpane_focus_colour(void)
{
	uint32_t c = 0;

	FYAI_TCHECK(fyai_ui_color_parse("#1c2b3a", &c));
	FYAI_TCHECK(c == 0x1c2b3au);
	/* the hash is optional */
	c = 0;
	FYAI_TCHECK(fyai_ui_color_parse("1c2b3a", &c));
	FYAI_TCHECK(c == 0x1c2b3au);
	/* and every other shape is refused */
	FYAI_TCHECK(!fyai_ui_color_parse(NULL, &c));
	FYAI_TCHECK(!fyai_ui_color_parse("", &c));
	FYAI_TCHECK(!fyai_ui_color_parse("#1c2b3", &c));
	FYAI_TCHECK(!fyai_ui_color_parse("#1c2b3a7", &c));
	FYAI_TCHECK(!fyai_ui_color_parse("nonsense", &c));
	FYAI_TCHECK(!fyai_ui_color_parse("#1c2b3g", &c));
	FYAI_TCHECK(c == 0x1c2b3au);

	/*
	 * A ground can also be named rather than given, for a terminal whose
	 * own colours are the ones to stand on.
	 */
	FYAI_TCHECK(fyai_ui_ground_parse("reverse", &c));
	FYAI_TCHECK(c == FYTIM_COLOR_REVERSED);
	FYAI_TCHECK(fyai_ui_ground_parse("#1c2b3a", &c));
	FYAI_TCHECK(c == 0x1c2b3au);
	FYAI_TCHECK(!fyai_ui_ground_parse("reversed", &c));
	FYAI_TCHECK(!fyai_ui_ground_parse("", &c));

	/* The default is a ground, and one the terminal names. */
	memset(&wpt_cfg, 0, sizeof(wpt_cfg));
	fyai_config_set_defaults(&wpt_cfg);
	FYAI_TCHECK(fyai_ui_ground_parse(wpt_cfg.focus_bg, &c));
	FYAI_TCHECK(c == FYTIM_COLOR_REVERSED);
	return 0;
}

/* Every cell of @g is covered by exactly one tile: no hole, no overlap. */
static int wpt_grid_is_whole(const struct fyai_workpane_grid *g, int n)
{
	int seen[FYAI_WORKPANE_GRID_MAX][FYAI_WORKPANE_GRID_MAX];
	int i, r, c;

	memset(seen, 0, sizeof(seen));
	for (i = 0; i < n; i++) {
		const struct fyai_workpane_place *p = &g->place[i];

		if (p->row < 0 || p->col < 0 ||
		    p->row + p->row_span > g->rows ||
		    p->col + p->col_span > g->cols)
			return 0;
		for (r = p->row; r < p->row + p->row_span; r++)
			for (c = p->col; c < p->col + p->col_span; c++)
				if (seen[r][c]++)
					return 0;
	}
	for (r = 0; r < g->rows; r++)
		for (c = 0; c < g->cols; c++)
			if (!seen[r][c])
				return 0;
	return 1;
}

/*
 * A screen opened beside others takes a share of the pane, not a cell of a
 * square that happens to have one spare: three tiles are two and a wide one,
 * never two and a hole. The arrangement is stated for any number of them.
 */
int workpane_grid_has_no_holes(void)
{
	struct fyai_workpane_tile_info info[FYAI_WORKPANE_TILES_MAX];
	struct fyai_workpane_manager *wm = wpt_open("full", 0);
	struct fyai_workpane_grid grid;
	int i, n;

	memset(info, 0, sizeof(info));
	for (i = 0; i < FYAI_WORKPANE_TILES_MAX; i++) {
		info[i].kind = FYAI_WORKPANE_TILE_SHELL;
		info[i].preferred_rows = FYAI_WORKPANE_FILL;
	}
	for (n = 1; n <= 8; n++) {
		FYAI_TCHECK(!fyai_workpane_place(wm, info, n, &grid));
		FYAI_TCHECK(wpt_grid_is_whole(&grid, n));
	}

	/* Three of them: two side by side and one under both. */
	FYAI_TCHECK(!fyai_workpane_place(wm, info, 3, &grid));
	FYAI_TCHECK(grid.rows == 2 && grid.cols == 2);
	FYAI_TCHECK(grid.place[2].col_span == 2);

	/* A terminal too narrow to tile stacks them, which has no hole either. */
	fyai_workpane_terminal_resize(wm, 40, 30);
	wpt_cfg.work_min_tile_cols = 40;
	for (n = 1; n <= 5; n++) {
		FYAI_TCHECK(!fyai_workpane_place(wm, info, n, &grid));
		FYAI_TCHECK(grid.cols == 1);
		FYAI_TCHECK(wpt_grid_is_whole(&grid, n));
	}
	wpt_close(wm);
	return 0;
}

/*
 * A tile is placed where its age says. The pane holds its tiles oldest
 * first, and an arrangement that names one - the whole top row, the column
 * of its own - means the one that was there first; a tile stated in the
 * other order puts the newest screen where the oldest belongs, and moves
 * every screen when one opens.
 */
int workpane_tiles_in_age(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);
	struct fyai_workpane_grid grid;

	wpt_register_pair(wm);		/* the shell first, then the agent */
	/* The pane holds its tiles oldest first, so a walk of them starts at
	 * the shell: it was there before the agent. */
	FYAI_TCHECK(fyai_workpane_focus_next(wm));
	FYAI_TCHECK(fyai_workpane_focused(wm) == WPT_SHELL);
	FYAI_TCHECK(fyai_workpane_focus_next(wm));
	FYAI_TCHECK(fyai_workpane_focused(wm) == WPT_AGENT);
	fyai_workpane_clear_focus(wm);

	/* An arrangement that names one tile means that one. */
	fyai_workpane_set_layout(wm, FYAI_WORKPANE_LAYOUT_MAIN_TOP, 0);
	FYAI_TCHECK(!fyai_workpane_place(wm, NULL, 0, &grid));
	FYAI_TCHECK(grid.place[0].row == 0);
	FYAI_TCHECK(grid.place[1].row == 1);
	wpt_close(wm);
	return 0;
}

/* What the router was given, and for which tile. */
static char wpt_keys[64];
static size_t wpt_keys_len;

static void wpt_keys_router(void *user, const char *data, size_t len)
{
	(void)user;
	if (len > sizeof(wpt_keys) - wpt_keys_len)
		len = sizeof(wpt_keys) - wpt_keys_len;
	memcpy(wpt_keys + wpt_keys_len, data, len);
	wpt_keys_len += len;
}

/*
 * A key this program does not keep for itself belongs to the program that
 * holds them. ^C is the plainest of them: the terminal of the parent turns
 * it into a signal, and a signal to the parent is not what the user meant
 * when they were typing into a program.
 */
int workpane_keys_reach_program(void)
{
	struct fyai_workpane_manager *wm = wpt_open("full", 0);

	wpt_register_pair(wm);
	fyai_workpane_set_keys_router(wm, wpt_keys_router, NULL);

	/* With the keys at the prompt there is nothing to give them to. */
	wpt_keys_len = 0;
	FYAI_TCHECK(!fyai_workpane_keys_deliver(wm, "\x03", 1));
	FYAI_TCHECK(wpt_keys_len == 0);

	/* A tile that holds them is given the bytes as typed. */
	fyai_workpane_set_focus(wm, WPT_SHELL);
	FYAI_TCHECK(fyai_workpane_keys_deliver(wm, "\x03", 1));
	FYAI_TCHECK(wpt_keys_len == 1 && wpt_keys[0] == 3);

	/* And they stop when the keys go back to the prompt. */
	wpt_keys_len = 0;
	fyai_workpane_clear_focus(wm);
	FYAI_TCHECK(!fyai_workpane_keys_deliver(wm, "\x03", 1));
	FYAI_TCHECK(wpt_keys_len == 0);
	wpt_close(wm);
	return 0;
}
