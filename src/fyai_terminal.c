/*
 * fyai_terminal.c - terminal capabilities and theme detection
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fyai_terminal.h"

int markdown_render_width(void)
{
	struct winsize ws;
	const char *env;
	int width;

	/*
	 * markdown renderers may fill lines (notably code-block backgrounds) to the
	 * width it is given. Handing it the full terminal width makes those
	 * lines touch the right edge, where the terminal arms a pending
	 * auto-wrap; the trailing newline then lands on the already-
	 * wrapped next line and shows up as a spurious blank line. Reserve one
	 * column so a filled line never reaches the edge.
	 */
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 1)
		return ws.ws_col - 1;
	env = getenv("COLUMNS");
	if (!env || !*env)
		return 0;
	width = atoi(env);
	return width > 0 ? width : 0;
}

/* Terminal rows, 0 when unknown. */
int markdown_render_height(void)
{
	struct winsize ws;
	const char *env;
	int height;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
		return ws.ws_row;
	env = getenv("LINES");
	if (!env || !*env)
		return 0;
	height = atoi(env);
	return height > 0 ? height : 0;
}

/*
 * True terminal geometry for @fd, with no column reserved. The PTY that the
 * tool subsystem gives a command must match the window of the user. This is
 * therefore separate from markdown_render_width(), which keeps one column for
 * the renderer.
 */
bool terminal_window_size(int fd, int *rowsp, int *colsp)
{
	struct winsize ws;
	int rc;

	rc = ioctl(fd, TIOCGWINSZ, &ws);
	if (rc || !ws.ws_row || !ws.ws_col)
		return false;

	if (rowsp)
		*rowsp = ws.ws_row;
	if (colsp)
		*colsp = ws.ws_col;
	return true;
}

bool terminal_is_tty(int fd)
{
	return isatty(fd) == 1;
}

bool ansi_color_on(const char *color, int fd)
{
	if (color && !strcmp(color, "on"))
		return true;
	if (color && !strcmp(color, "off"))
		return false;
	return terminal_is_tty(fd);
}

bool markdown_color_enabled(const char *color)
{
	return ansi_color_on(color, STDOUT_FILENO);
}

bool terminal_text_at_line_start(const char *text, size_t len)
{
	size_t i;
	bool line_start;

	line_start = true;
	for (i = 0; i < len; i++) {
		if (text[i] == '\033' && i + 1 < len && text[i + 1] == '[') {
			i += 2;
			while (i < len &&
			       ((unsigned char)text[i] < 0x40 ||
				(unsigned char)text[i] > 0x7e))
				i++;
			continue;
		}
		if (text[i] == '\n' || text[i] == '\r')
			line_start = true;
		else
			line_start = false;
	}
	return line_start;
}

size_t terminal_trim_blank_rows(const char *text, size_t len)
{
	size_t end;
	size_t line_end;
	size_t start;
	size_t i;
	bool blank;

	end = len;
	while (end) {
		line_end = end;
		while (line_end &&
		       (text[line_end - 1] == '\n' ||
			text[line_end - 1] == '\r'))
			line_end--;
		start = line_end;
		while (start && text[start - 1] != '\n')
			start--;
		blank = true;
		for (i = start; i < line_end; i++) {
			if (text[i] == '\033' && i + 1 < line_end &&
			    text[i + 1] == '[') {
				i += 2;
				while (i < line_end &&
				       ((unsigned char)text[i] < 0x40 ||
					(unsigned char)text[i] > 0x7e))
					i++;
				continue;
			}
			if (text[i] != '\r' && text[i] != ' ' &&
			    text[i] != '\t') {
				blank = false;
				break;
			}
		}
		if (!blank)
			break;
		end = start;
	}
	return end;
}
