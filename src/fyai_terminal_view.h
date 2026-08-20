/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TERMINAL_VIEW_H
#define FYAI_TERMINAL_VIEW_H

#include <stdbool.h>
#include <stddef.h>

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

/*
 * Read the view. The caller owns the returned string. @region applies to
 * FYAITR_REGION only. FYAITR_NEW moves the read mark; the other reads do not.
 */
char *fyai_terminal_view_read(struct fyai_terminal_view *view,
			      enum fyai_terminal_read what,
			      const struct fyai_terminal_region *region,
			      size_t *lenp);

#endif
