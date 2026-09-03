/*
 * fyai_workpane.h - the one owner of work-pane geometry, focus, and zoom
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FYAI_WORKPANE_H
#define FYAI_WORKPANE_H

#include <stdbool.h>

#include "fyai_ui.h"

struct fyai_ctx;
struct fytim;
struct fytim_surface;
struct fytim_workband;
struct fytim_workpane;
struct fyai_workpane_manager;

/* The one component that writes a pane cap or a tile request. A producer
 * states intent through the operations below and sizes nothing itself. */

enum fyai_workpane_mode {
	FYAI_WORKPANE_NORMAL,
	FYAI_WORKPANE_ZOOMED,
};

/*
 * The pane height policy. FULL is an active disposition, not an absent one:
 * it becomes the layout library's uncapped zero only at the API boundary.
 */
enum fyai_workpane_disposition {
	FYAI_WORKPANE_FULL,
	FYAI_WORKPANE_HALF,
	FYAI_WORKPANE_QUARTER,
	FYAI_WORKPANE_FIXED,
};

/* The most tiles a policy places, and the largest grid it may ask for. */
#define FYAI_WORKPANE_TILES_MAX 32
#define FYAI_WORKPANE_GRID_MAX 16

/* How the pane arranges its tiles. AUTO fits as many equal, readable
 * columns as it can; the rest are arrangements this program states. */
enum fyai_workpane_layout {
	FYAI_WORKPANE_LAYOUT_AUTO,
	FYAI_WORKPANE_LAYOUT_STACK,	/* one screen above another */
	FYAI_WORKPANE_LAYOUT_COLUMNS,	/* a fixed number of columns */
	FYAI_WORKPANE_LAYOUT_MAIN_TOP,	/* one wide screen over the rest */
	FYAI_WORKPANE_LAYOUT_MAIN_LEFT,	/* one tall screen beside the rest */
	FYAI_WORKPANE_LAYOUT_CUSTOM,	/* the policy callback places them */
};

/*
 * Where the pane stands. It is the last band above the prompt by default,
 * which keeps the work next to the transcript it came from. BELOW takes the
 * rows under the prompt instead: the user types over the work, and a pane
 * that grows moves nothing that is being read.
 */
enum fyai_workpane_pos {
	FYAI_WORKPANE_POS_ABOVE,
	FYAI_WORKPANE_POS_BELOW,
};

/*
 * What a tile draws at the size it was given. A screen that cannot be read
 * is worth less than the one line that says whose it is, so a tile too small
 * for its program shows its head and what the program is doing instead.
 */
enum fyai_workpane_present {
	FYAI_WORKPANE_PRESENT_FULL,	/* the screen of the program */
	FYAI_WORKPANE_PRESENT_OUTPUT,	/* the screen, without its head */
	FYAI_WORKPANE_PRESENT_HEAD,	/* the head and an activity mark */
	FYAI_WORKPANE_PRESENT_HIDDEN,	/* the tile was granted nothing */
};

/*
 * The sizes at which a tile changes what it draws. A tile is FULL at or above
 * @full_rows by @full_cols, OUTPUT while it still has @output_rows, and HEAD
 * below that. Zero in a field leaves that step out of the ladder.
 */
struct fyai_workpane_ladder {
	int full_rows, full_cols;
	int output_rows;
};

/* Where a policy put one tile. */
struct fyai_workpane_place {
	int row, col, row_span, col_span;
};

/* The arrangement a policy chose. */
struct fyai_workpane_grid {
	int rows, cols;
	/* 0 shares what the sized tracks leave; FYAI_WORKPANE_TRACK_FIT sizes
	 * the track to its tiles and takes those cells first. */
#define FYAI_WORKPANE_TRACK_FIT (-1)
	int row_size[FYAI_WORKPANE_GRID_MAX];
	int col_size[FYAI_WORKPANE_GRID_MAX];
	struct fyai_workpane_place place[FYAI_WORKPANE_TILES_MAX];
};

/* What a tile holds. The first three tile beside one another as work; a
 * notice is a report to the user and takes a row of its own under them. */
enum fyai_workpane_tile_kind {
	FYAI_WORKPANE_TILE_SHELL,
	FYAI_WORKPANE_TILE_AGENT,
	FYAI_WORKPANE_TILE_TEXT,
	FYAI_WORKPANE_TILE_NOTICE,
};

/* True when @kind is work that tiles beside other work. */
static inline bool fyai_workpane_kind_is_work(enum fyai_workpane_tile_kind k)
{
	return k != FYAI_WORKPANE_TILE_NOTICE;
}

/* What a policy is told about one tile, in registration order. */
struct fyai_workpane_tile_info {
	enum fyai_workpane_tile_kind kind;
	int preferred_rows;		/* the height it asks layout for */
	bool focused;
};

/*
 * A policy places @n tiles. It fills @out with the grid it wants and one
 * placement per tile, and returns 0. A policy that returns non-zero leaves
 * the arrangement to the library.
 */
typedef int (*fyai_workpane_policy_fn)(void *user,
				       const struct fyai_workpane_tile_info *tiles,
				       int n, int term_rows, int term_cols,
				       struct fyai_workpane_grid *out);

/* What the manager asks of the component that owns a tile's program. */
struct fyai_workpane_tile_ops {
	/* Apply a layout grant to the program's grid. The owner may size a
	 * pseudo-terminal and publish; it must not request rows or set a cap. */
	void (*apply_grant)(void *owner, int rows, int cols);
	/* The tile lost keyboard focus, or took it. Chrome only. */
	void (*focus_changed)(void *owner, bool focused);
	/* Draw this much of the program: the tile is too small for the rest. */
	void (*set_presentation)(void *owner, enum fyai_workpane_present p);
};

struct fyai_workpane_manager *fyai_workpane_create(struct fyai_ctx *ctx,
						   struct fytim *ft);
void fyai_workpane_destroy(struct fyai_workpane_manager *wm);

/*
 * The pane holding every tile. It is created with the first tile and retired
 * with the last, so a session that runs no program shows no pane.
 */
struct fytim_workpane *fyai_workpane_acquire(struct fyai_workpane_manager *wm);
void fyai_workpane_release(struct fyai_workpane_manager *wm);
/* Apply grid and chrome configuration to an existing pane. */
void fyai_workpane_configure(struct fyai_workpane_manager *wm);

/*
 * Register a live tile. @preferred_rows is the height the tile asks layout
 * for, independent of what it has drawn; negative prefers the whole pane,
 * zero leaves the request to the library. @max_rows is a tile policy; zero
 * lifts it.
 */
int fyai_workpane_register(struct fyai_workpane_manager *wm,
			   struct fytim_surface *sf,
			   enum fyai_workpane_tile_kind kind, void *owner,
			   const struct fyai_workpane_tile_ops *ops,
			   int preferred_rows, int max_rows);
/* Register a band tile, which the manager sizes only through the pane. */
int fyai_workpane_register_band(struct fyai_workpane_manager *wm,
				struct fytim_workband *band,
				enum fyai_workpane_tile_kind kind, void *owner);
/* Retire a tile. Focus and zoom that named it are cleared in the same pass. */
void fyai_workpane_unregister(struct fyai_workpane_manager *wm,
			      struct fytim_surface *sf);
void fyai_workpane_unregister_band(struct fyai_workpane_manager *wm,
				   struct fytim_workband *band);

/* The component that owns @sf, or NULL. */
void *fyai_workpane_tile_owner(const struct fyai_workpane_manager *wm,
			       const struct fytim_surface *sf,
			       enum fyai_workpane_tile_kind *kindp);

/*
 * Where the bytes of the focused tile go: one router serves every tile, set
 * once by the component that owns the programs. Give @len bytes to the tile
 * that holds the keys, as if the user had typed them at it. Returns false
 * when the keys are the prompt's, so a caller can act on them itself. It is
 * how a key the terminal of this process took for itself - ^C, which arrives
 * as a signal - reaches the program it was meant for.
 */
bool fyai_workpane_keys_deliver(struct fyai_workpane_manager *wm,
				const char *data, size_t len);
void fyai_workpane_set_keys_router(struct fyai_workpane_manager *wm,
				   fyai_ui_keys_fn cb, void *user);

/* Prefer the whole pane, as a user-owned program does. */
#define FYAI_WORKPANE_FILL (-1)

/* Choose the arrangement. @columns applies to FYAI_WORKPANE_LAYOUT_COLUMNS. */
void fyai_workpane_set_layout(struct fyai_workpane_manager *wm,
			      enum fyai_workpane_layout layout, int columns);
enum fyai_workpane_layout
fyai_workpane_layout(const struct fyai_workpane_manager *wm);
/* Choose where the pane stands against the prompt. */
void fyai_workpane_set_position(struct fyai_workpane_manager *wm,
				enum fyai_workpane_pos pos);
enum fyai_workpane_pos
fyai_workpane_position(const struct fyai_workpane_manager *wm);
/* Place the tiles with @fn. Selecting CUSTOM without one falls back to AUTO. */
void fyai_workpane_set_policy(struct fyai_workpane_manager *wm,
			      fyai_workpane_policy_fn fn, void *user);
/* The sizes at which @sf changes what it draws. */
void fyai_workpane_tile_set_ladder(struct fyai_workpane_manager *wm,
				   struct fytim_surface *sf,
				   const struct fyai_workpane_ladder *ladder);
/*
 * Solve the arrangement for @n tiles described by @info, which is the tiles
 * the manager holds when @info is NULL. Returns 0 and fills @out, or non-zero
 * when the layout leaves the arrangement to the library.
 */
int fyai_workpane_place(struct fyai_workpane_manager *wm,
			const struct fyai_workpane_tile_info *info, int n,
			struct fyai_workpane_grid *out);

/* What @sf would draw if it were granted @rows by @cols. */
enum fyai_workpane_present
fyai_workpane_present_at(const struct fyai_workpane_manager *wm,
			 const struct fytim_surface *sf, int rows, int cols);

/* What @sf was last told to draw. */
enum fyai_workpane_present
fyai_workpane_tile_presentation(const struct fyai_workpane_manager *wm,
				const struct fytim_surface *sf);

/* Say whether @sf may still take focus. A program that ended may not. */
void fyai_workpane_tile_set_selectable(struct fyai_workpane_manager *wm,
				       struct fytim_surface *sf, bool ok);

/* Keyboard focus. Focus never changes pane or tile geometry. */
void fyai_workpane_set_focus(struct fyai_workpane_manager *wm,
			     struct fytim_surface *sf);
void fyai_workpane_clear_focus(struct fyai_workpane_manager *wm);
/* Move to the next live tile, then to the prompt. True when focus moved. */
bool fyai_workpane_focus_next(struct fyai_workpane_manager *wm);
struct fytim_surface *
fyai_workpane_focused(const struct fyai_workpane_manager *wm);

/* Zoom. Zoom selects the only visible tile and preserves focus. */
int fyai_workpane_set_zoom(struct fyai_workpane_manager *wm,
			   struct fytim_surface *sf);
void fyai_workpane_clear_zoom(struct fyai_workpane_manager *wm);
struct fytim_surface *
fyai_workpane_zoomed(const struct fyai_workpane_manager *wm);

/* The terminal changed size. Fractional dispositions follow it. */
void fyai_workpane_terminal_resize(struct fyai_workpane_manager *wm, int rows,
				   int cols);
/* Set the pane height policy. @fixed_rows applies to FYAI_WORKPANE_FIXED. */
void fyai_workpane_set_disposition(struct fyai_workpane_manager *wm,
				   enum fyai_workpane_disposition d,
				   int fixed_rows);
enum fyai_workpane_disposition
fyai_workpane_disposition(const struct fyai_workpane_manager *wm);
/* The row cap the last reconciliation resolved; zero is uncapped. */
int fyai_workpane_resolved_rows(const struct fyai_workpane_manager *wm);
/* The height @sf asks layout for, which its content never changes. */
int fyai_workpane_tile_preferred_rows(const struct fyai_workpane_manager *wm,
				      const struct fytim_surface *sf);
/* The rows layout last granted @sf. */
int fyai_workpane_tile_granted_rows(const struct fyai_workpane_manager *wm,
				    const struct fytim_surface *sf);

/* Adopt the disposition the configuration states. */
void fyai_workpane_adopt_config(struct fyai_workpane_manager *wm);
/* Advance the pane height policy and report it. */
void fyai_workpane_cycle_disposition(struct fyai_workpane_manager *wm);

/* Bring the library's state to the manager's. Idempotent and not reentrant. */
void fyai_workpane_reconcile(struct fyai_workpane_manager *wm);
/* Read grants after a layout pass and hand them to tile owners. */
void fyai_workpane_layout_complete(struct fyai_workpane_manager *wm);
/* Record the grid a program acknowledged. Layout intent is unchanged. */
void fyai_workpane_grid_resized(struct fyai_workpane_manager *wm,
				struct fytim_surface *sf, int rows, int cols);

/* True when the configured tile controls need the mouse. */
bool fyai_workpane_wants_mouse(const struct fyai_ctx *ctx);

/* The manager of @ctx, or NULL without an interactive display. */
struct fyai_workpane_manager *fyai_workpane_of(const struct fyai_ctx *ctx);

#endif
