/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TERMINAL_VIEW_H
#define FYAI_TERMINAL_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils.h"

/* The size used when no argument, setting, or real terminal supplies one. */
#define FYAI_TTY_ROWS_DEFAULT	24
#define FYAI_TTY_COLS_DEFAULT	80

/* A command's plain line log and interpreted terminal screen. */
struct fyai_terminal_view;
struct fyai_ctx;

enum fyai_terminal_read {
	FYAITR_NEW,	/* what appeared since the previous read */
	FYAITR_SCREEN,	/* the visible screen */
	FYAITR_ALL,	/* everything the command has produced */
	FYAITR_REGION,	/* a rectangle of the visible screen */
};

struct fyai_terminal_region {
	int row;
	int col;
	int rows;
	int cols;
};

struct fyai_terminal_view *fyai_terminal_view_create(struct fyai_ctx *ctx,
						     int rows, int cols,
						     size_t max_bytes);
void fyai_terminal_view_destroy(struct fyai_terminal_view *view);

/* Give the view the bytes the program wrote to its terminal. */
int fyai_terminal_view_feed(struct fyai_terminal_view *view, const char *data,
			    size_t len);
void fyai_terminal_view_resize(struct fyai_terminal_view *view, int rows,
			       int cols);

/* Report completed lines until the program starts addressing the screen. */
void fyai_terminal_view_line_cb(struct fyai_terminal_view *view,
				shell_output_fn cb, void *userdata);

bool fyai_terminal_view_screen_mode(const struct fyai_terminal_view *view);
bool fyai_terminal_view_binary(const struct fyai_terminal_view *view);
size_t fyai_terminal_view_raw_bytes(const struct fyai_terminal_view *view);
void fyai_terminal_view_size(const struct fyai_terminal_view *view, int *rowsp,
			     int *colsp);


/* Public cell and damage types for rendering an interpreted screen. */
#define FYAI_TERM_CELL_CHARS	6

struct fyai_term_color {
	bool is_default;	/* the colour of the terminal, not one chosen */
	unsigned char r, g, b;
};

struct fyai_term_cell {
	uint32_t chars[FYAI_TERM_CELL_CHARS];	/* base then combining */
	int width;				/* 2 for a double-width glyph */
	struct fyai_term_color fg;
	struct fyai_term_color bg;
	bool bold;
	bool underline;
	bool italic;
	bool blink;
	bool reverse;
	bool strike;
};

/* One cell of the visible screen. False when @row or @col is outside it. */
bool fyai_terminal_view_cell(const struct fyai_terminal_view *view, int row,
			     int col, struct fyai_term_cell *cell);

/* Where the program left its cursor, and whether it asked for it to show. */
void fyai_terminal_view_cursor(const struct fyai_terminal_view *view,
			       int *rowp, int *colp, bool *visiblep);

/*
 * The rows that changed since the previous call, inclusive, and false when
 * nothing did. Taking the damage clears it, so a renderer paints each
 * change one time.
 */
bool fyai_terminal_view_take_damage(struct fyai_terminal_view *view,
				    int *firstp, int *lastp);

/* True when a paint has work: a changed row, or a cursor that moved. */
bool fyai_terminal_view_dirty(const struct fyai_terminal_view *view);

/* Mark the whole screen as changed; the next paint draws all of it. */
void fyai_terminal_view_damage_all(struct fyai_terminal_view *view);

/* Treat a bare line feed as a complete newline. */
void fyai_terminal_view_cooked(struct fyai_terminal_view *view, bool cooked);

/*
 * Read the view. The caller owns the returned string. @region applies to
 * FYAITR_REGION only. FYAITR_NEW moves the read mark; the other reads do not.
 */
char *fyai_terminal_view_read(struct fyai_terminal_view *view,
			      enum fyai_terminal_read what,
			      const struct fyai_terminal_region *region,
			      size_t *lenp);

/* Return the owned prompt row nearest the cursor, or NULL. */
char *fyai_terminal_view_last_line(const struct fyai_terminal_view *view);

#endif
