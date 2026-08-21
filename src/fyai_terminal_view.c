/*
 * fyai_terminal_view.c - the terminal state of one command
 *
 * The process that reads the output of a command owns this. It keeps a line
 * log and an interpreted screen at the same time. Refer to
 * fyai_terminal_view.h for the reason.
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_TOOLS
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libfyvterm.h>
#include "fyai_diag.h"
#include "fyai_terminal_view.h"
#include "utils.h"

/* The escape-sequence reader of the line log. */
enum tty_scan {
	TTYSC_TEXT,
	TTYSC_ESC,		/* after ESC */
	TTYSC_CSI,		/* after ESC [ */
	TTYSC_STR,		/* OSC, DCS, APC or PM, until ST or BEL */
	TTYSC_STR_ESC,		/* after ESC inside a string */
	TTYSC_SKIP,		/* one byte of a two-byte escape */
};

struct fyai_terminal_view {
	struct fyai_ctx *ctx;
	struct fyvt *vt;
	struct fyvt_screen *screen;
	struct response_buffer sb;	/* rows that left the top of the screen */
	struct response_buffer log;	/* the line log, sequences removed */
	size_t log_line;		/* where the line being written begins */
	size_t log_read;		/* how much of the log was read */
	size_t max_bytes;		/* retained text, 0 = no limit */
	size_t raw_bytes;
	shell_output_fn line_cb;
	void *line_data;
	size_t line_sent;		/* how much of the log was reported */
	enum tty_scan scan;
	bool pending_cr;		/* a carriage return waiting for its byte */
	char csi[32];			/* the parameters of the sequence */
	size_t csi_len;
	int rows;
	int cols;
	/* Rows and cursor state needed for the next surface repaint. */
	int damage_first;
	int damage_last;
	int cursor_row;
	int cursor_col;
	bool cursor_visible;
	bool cursor_moved;	/* a paint must place the cursor again */
	bool screen_mode;
	bool binary;
};

/* Make a bare line feed advance to the next line's first column. */
void fyai_terminal_view_cooked(struct fyai_terminal_view *view, bool cooked)
{
	static const char set[] = "\x1b[20h";
	static const char reset[] = "\x1b[20l";

	if (!view)
		return;
	fyai_terminal_view_feed(view, cooked ? set : reset,
				(cooked ? sizeof(set) : sizeof(reset)) - 1);
	/* These bytes are ours, not the program's: it wrote nothing yet. */
	view->raw_bytes = 0;
}

void fyai_terminal_view_damage_all(struct fyai_terminal_view *view);

/* Remember that rows [@first, @last] must be painted again. */
static void tty_damage(struct fyai_terminal_view *view, int first, int last)
{
	if (first < 0)
		first = 0;
	if (last >= view->rows)
		last = view->rows - 1;
	if (first > last)
		return;
	if (view->damage_first > view->damage_last) {
		view->damage_first = first;
		view->damage_last = last;
		return;
	}
	if (first < view->damage_first)
		view->damage_first = first;
	if (last > view->damage_last)
		view->damage_last = last;
}

/* Encode one code point. @out holds at least 4 bytes. */
static size_t tty_put_utf8(char *out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

/* Append a row as text, preserving combining and double-width characters. */
static int tty_append_cells(struct response_buffer *out,
			    const struct fyvt_screen_cell *cells, int cols)
{
	char utf8[4];
	size_t n;
	int col, last, i;
	int rc;

	last = -1;
	for (col = 0; col < cols; col++)
		if (cells[col].chars[0])
			last = col;

	for (col = 0; col <= last; col++) {
		if (!cells[col].chars[0]) {
			rc = response_buffer_append_data(out, " ", 1);
			if (rc)
				return -1;
			continue;
		}
		for (i = 0; i < FYVT_MAX_CHARS_PER_CELL; i++) {
			if (!cells[col].chars[i])
				break;
			n = tty_put_utf8(utf8, cells[col].chars[i]);
			rc = response_buffer_append_data(out, utf8, n);
			if (rc)
				return -1;
		}
		if (cells[col].width > 1)
			col += cells[col].width - 1;
	}
	return 0;
}

/* Return the nearest nonempty row at or above the cursor. */
char *fyai_terminal_view_last_line(const struct fyai_terminal_view *view)
{
	struct response_buffer buf = {};
	struct fyvt_screen_cell *cells;
	struct fyvt_pos pos;
	char *line;
	int row, col;

	if (!view || view->cols <= 0)
		return NULL;
	cells = calloc((size_t)view->cols, sizeof(*cells));
	if (!cells)
		return NULL;

	for (row = view->cursor_row; row >= 0; row--) {
		for (col = 0; col < view->cols; col++) {
			pos.row = row;
			pos.col = col;
			fyvt_screen_get_cell(view->screen, pos, &cells[col]);
		}
		if (tty_append_cells(&buf, cells, view->cols))
			break;
		if (buf.len)
			break;
	}
	free(cells);
	line = buf.len ? buf.data : NULL;
	if (!line)
		free(buf.data);
	return line;
}

/* Bound retained text by dropping old complete lines. */
static size_t tty_compact(struct response_buffer *buf, size_t max_bytes)
{
	size_t drop, keep;
	char *nl;

	if (!max_bytes || !buf->data || buf->len <= max_bytes * 2)
		return 0;

	nl = memchr(buf->data + buf->len - max_bytes, '\n', max_bytes);
	keep = nl ? buf->len - (size_t)(nl + 1 - buf->data) : max_bytes;
	drop = buf->len - keep;
	memmove(buf->data, buf->data + drop, keep);
	buf->len = keep;
	buf->data[keep] = '\0';
	return drop;
}

static void tty_mark_drop(size_t *mark, size_t drop)
{
	*mark = *mark > drop ? *mark - drop : 0;
}

/* Retain a scrolled row, joining wrapped rows into their original line. */
static int tty_sb_pushline4(int cols, const struct fyvt_screen_cell *cells,
			    bool continuation, void *user)
{
	struct fyai_terminal_view *view = user;

	if (!continuation && view->sb.len &&
	    response_buffer_append_data(&view->sb, "\n", 1))
		return 0;
	if (tty_append_cells(&view->sb, cells, cols))
		return 0;
	(void)tty_compact(&view->sb, view->max_bytes);
	return 1;
}

static int tty_cb_damage(struct fyvt_rect rect, void *user)
{
	struct fyai_terminal_view *view = user;

	tty_damage(view, rect.start_row, rect.end_row - 1);
	return 1;
}

static int tty_cb_movecursor(struct fyvt_pos pos, struct fyvt_pos oldpos,
			     int visible, void *user)
{
	struct fyai_terminal_view *view = user;

	(void)oldpos;
	view->cursor_row = pos.row;
	view->cursor_col = pos.col;
	view->cursor_visible = !!visible;
	view->cursor_moved = true;
	return 1;
}

/* Record cursor visibility for the surface renderer. */
static int tty_cb_settermprop(enum fyvt_prop prop, union fyvt_value *val,
			      void *user)
{
	struct fyai_terminal_view *view = user;

	if (prop == FYVT_PROP_CURSORVISIBLE) {
		view->cursor_visible = val->boolean;
		view->cursor_moved = true;
	}
	return 1;
}

/* Without a moverect callback, libfyvterm damages moved rows for us. */
static const struct fyvt_screen_callbacks tty_screen_callbacks = {
	.damage = tty_cb_damage,
	.movecursor = tty_cb_movecursor,
	.settermprop = tty_cb_settermprop,
	.sb_pushline4 = tty_sb_pushline4,
};

/* True when screen row @row continues the line above it. */
static bool tty_row_continues(struct fyai_terminal_view *view, int row)
{
	const struct fyvt_line_info *info;

	info = fyvt_state_get_lineinfo(fyvt_obtain_state(view->vt), row);
	return info && info->continuation;
}

/* Send each completed line to the live display, while there are lines. */
static void tty_line_report(struct fyai_terminal_view *view)
{
	size_t end;

	if (!view->line_cb || view->screen_mode)
		return;
	/* Report up to the end of the last completed line. */
	end = view->log_line;
	if (end <= view->line_sent)
		return;
	view->line_cb(view->line_data, SHELL_OUTPUT_STDOUT,
		      view->log.data + view->line_sent, end - view->line_sent);
	view->line_sent = end;
}

/* Decide whether an escape sequence changes lines or addresses the screen. */
static bool tty_csi_draws(const char *seq, size_t len, char final)
{
	static const char draw[] = "ABCDEFGHfdJrsu";
	unsigned long param;
	char *end;

	if (final && strchr(draw, final))
		return true;
	if (final != 'h' && final != 'l')
		return false;
	if (!len || seq[0] != '?')
		return false;

	/* A private mode: the alternate screen and mouse reporting draw. */
	param = strtoul(seq + 1, &end, 10);
	switch (param) {
	case 47:
	case 1000:
	case 1002:
	case 1003:
	case 1006:
	case 1047:
	case 1048:
	case 1049:
		return true;
	default:
		break;
	}
	return false;
}

static void tty_log_putc(struct fyai_terminal_view *view, char c)
{
	if (response_buffer_append_data(&view->log, &c, 1))
		return;
}

/* Add one byte to the plain line log, applying overwrite controls. */
static void tty_log_feed(struct fyai_terminal_view *view, unsigned char c)
{
	size_t drop;

	switch (view->scan) {
	case TTYSC_TEXT:
		break;
	case TTYSC_ESC:
		if (c == '[') {
			view->scan = TTYSC_CSI;
			view->csi_len = 0;
		} else if (c == ']' || c == 'P' || c == 'X' || c == '^' ||
			   c == '_') {
			view->scan = TTYSC_STR;
		} else if (c == '(' || c == ')' || c == '*' || c == '+' ||
			   c == '#' || c == ' ') {
			view->scan = TTYSC_SKIP;
		} else {
			/* ESC M and ESC c move or reset the screen. */
			if (c == 'M' || c == 'c' || c == 'D' || c == 'E')
				view->screen_mode = true;
			view->scan = TTYSC_TEXT;
		}
		return;
	case TTYSC_CSI:
		if (c >= 0x40 && c <= 0x7e) {
			if (tty_csi_draws(view->csi, view->csi_len, (char)c))
				view->screen_mode = true;
			view->scan = TTYSC_TEXT;
			return;
		}
		if (view->csi_len < sizeof(view->csi) - 1)
			view->csi[view->csi_len++] = (char)c;
		view->csi[view->csi_len] = '\0';
		return;
	case TTYSC_STR:
		if (c == 0x07)
			view->scan = TTYSC_TEXT;
		else if (c == 0x1b)
			view->scan = TTYSC_STR_ESC;
		return;
	case TTYSC_STR_ESC:
		view->scan = c == 0x1b ? TTYSC_STR_ESC : TTYSC_TEXT;
		return;
	case TTYSC_SKIP:
		view->scan = TTYSC_TEXT;
		return;
	}

	/*
	 * A terminal turns a newline into a carriage return and a newline,
	 * so a carriage return is only an overwrite when another character
	 * follows it. Its meaning is decided by the byte after it.
	 */
	if (view->pending_cr) {
		view->pending_cr = false;
		if (c != '\n') {
			view->log.len = view->log_line;
			if (view->log.data)
				view->log.data[view->log.len] = '\0';
			if (view->log_read > view->log.len)
				view->log_read = view->log.len;
			if (view->line_sent > view->log.len)
				view->line_sent = view->log.len;
		}
	}

	switch (c) {
	case 0x1b:
		view->scan = TTYSC_ESC;
		return;
	case '\r':
		view->pending_cr = true;
		return;
	case '\n':
		tty_log_putc(view, '\n');
		view->log_line = view->log.len;
		tty_line_report(view);
		drop = tty_compact(&view->log, view->max_bytes);
		if (drop) {
			tty_mark_drop(&view->log_line, drop);
			tty_mark_drop(&view->log_read, drop);
			tty_mark_drop(&view->line_sent, drop);
		}
		return;
	case '\b':
		if (view->log.len > view->log_line) {
			view->log.len--;
			if (view->log.data)
				view->log.data[view->log.len] = '\0';
		}
		return;
	case '\t':
		tty_log_putc(view, '\t');
		return;
	default:
		break;
	}
	if (c < 0x20 || c == 0x7f)
		return;
	tty_log_putc(view, (char)c);
}

struct fyai_terminal_view *fyai_terminal_view_create(struct fyai_ctx *ctx,
						     int rows, int cols,
						     size_t max_bytes)
{
	struct fyai_terminal_view *view;
	struct fyvt_cfg vtcfg;

	view = calloc(1, sizeof(*view));
	fyai_error_check(ctx, view, err,
			 "terminal: could not allocate the view");

	view->ctx = ctx;
	view->rows = rows > 0 ? rows : FYAI_TTY_ROWS_DEFAULT;
	view->cols = cols > 0 ? cols : FYAI_TTY_COLS_DEFAULT;
	view->max_bytes = max_bytes;
	view->scan = TTYSC_TEXT;
	/* An empty range: first above last. Nothing is painted yet. */
	view->damage_first = 1;
	view->damage_last = 0;
	view->cursor_visible = true;

	fyvt_cfg_default(&vtcfg);
	vtcfg.rows = view->rows;
	vtcfg.cols = view->cols;
	view->vt = fyvt_create(&vtcfg);
	fyai_error_check(ctx, view->vt, err,
			 "terminal: could not create the emulator");
	/* fyvt_create() starts in 8-bit mode; without this every byte above 0x7f
	 * is taken as a separate character and text is mangled. */
	fyvt_set_utf8(view->vt, 1);
	view->screen = fyvt_obtain_screen(view->vt);
	fyvt_screen_set_callbacks(view->screen, &tty_screen_callbacks, view);
	/* Without this the ABI-compatible sb_pushline is called instead, and a
	 * line that needed several rows cannot be joined again. */
	fyvt_screen_callbacks_has_pushline4(view->screen);
	/*
	 * Reflow a line that a resize makes fit differently. It is what the
	 * program sees, and it keeps a joined line joined across a resize.
	 */
	fyvt_screen_enable_reflow(view->screen, true);
	/*
	 * A renderer paints whole rows, so cell damage would only make the
	 * screen layer report the same rows in more pieces.
	 */
	fyvt_screen_set_damage_merge(view->screen, FYVT_DAMAGE_ROW);
	fyvt_screen_reset(view->screen, 1);
	/* The reset damages the screen; a first paint draws all of it. */
	fyai_terminal_view_damage_all(view);
	return view;

err:
	if (view)
		fyai_terminal_view_destroy(view);
	return NULL;
}

void fyai_terminal_view_destroy(struct fyai_terminal_view *view)
{
	if (!view)
		return;
	if (view->vt)
		fyvt_destroy(view->vt);
	free(view->sb.data);
	free(view->log.data);
	free(view);
}

int fyai_terminal_view_feed(struct fyai_terminal_view *view, const char *data,
			    size_t len)
{
	size_t written;
	size_t i;

	if (!view || !data || !len)
		return 0;

	if (!view->binary && data_is_binary(data, len))
		view->binary = true;
	view->raw_bytes += len;

	/* The screen is interpreted from the same bytes as the line log. */
	written = fyvt_input_write(view->vt, data, len);
	fyai_error_check(view->ctx, written == len, err,
			 "terminal: could not interpret all input bytes");
	/*
	 * Damage of a row is merged until it is flushed. Without this the
	 * report of the row just written stays inside the screen layer, and a
	 * renderer draws everything except what appeared.
	 */
	fyvt_screen_flush_damage(view->screen);
	for (i = 0; i < len; i++)
		tty_log_feed(view, (unsigned char)data[i]);
	return 0;

err:
	return -1;
}

void fyai_terminal_view_resize(struct fyai_terminal_view *view, int rows,
			       int cols)
{
	if (!view || rows <= 0 || cols <= 0 ||
	    (rows == view->rows && cols == view->cols))
		return;
	view->rows = rows;
	view->cols = cols;
	fyvt_set_size(view->vt, rows, cols);
	fyai_terminal_view_damage_all(view);
}

void fyai_terminal_view_line_cb(struct fyai_terminal_view *view,
				shell_output_fn cb, void *userdata)
{
	if (!view)
		return;
	view->line_cb = cb;
	view->line_data = userdata;
}

bool fyai_terminal_view_screen_mode(const struct fyai_terminal_view *view)
{
	return view && view->screen_mode;
}

bool fyai_terminal_view_binary(const struct fyai_terminal_view *view)
{
	return view && view->binary;
}

size_t fyai_terminal_view_raw_bytes(const struct fyai_terminal_view *view)
{
	return view ? view->raw_bytes : 0;
}

void fyai_terminal_view_size(const struct fyai_terminal_view *view, int *rowsp,
			     int *colsp)
{
	if (!view)
		return;
	if (rowsp)
		*rowsp = view->rows;
	if (colsp)
		*colsp = view->cols;
}

/* Render rows [@first, @last) of the screen, joining continued rows. */
static char *tty_screen_text(struct fyai_terminal_view *view, int first,
			     int last, int col, int cols, bool with_sb,
			     size_t *lenp)
{
	struct response_buffer out = {};
	struct fyvt_screen_cell *cells;
	struct fyvt_pos pos;
	int row, i, rc;

	cells = calloc((size_t)view->cols, sizeof(*cells));
	fyai_error_check(view->ctx, cells, fail,
			 "terminal: could not allocate screen cells");

	if (with_sb && view->sb.len) {
		rc = response_buffer_append_data(&out, view->sb.data,
						  view->sb.len);
		fyai_error_check(view->ctx, !rc, fail,
				 "terminal: could not retain scrollback");
	}

	for (row = first; row < last; row++) {
		if (out.len && !tty_row_continues(view, row)) {
			rc = response_buffer_append_data(&out, "\n", 1);
			fyai_error_check(view->ctx, !rc, fail,
					 "terminal: could not join screen rows");
		}
		for (i = 0; i < cols; i++) {
			pos.row = row;
			pos.col = col + i;
			if (!fyvt_screen_get_cell(view->screen, pos,
						   &cells[i]))
				memset(&cells[i], 0, sizeof(cells[i]));
		}
		fyai_error_check(view->ctx,
				 !tty_append_cells(&out, cells, cols), fail,
				 "terminal: could not append screen cells");
	}
	free(cells);

	/* A screen is mostly blank at the bottom; do not send empty rows. */
	response_buffer_trim(&out);
	/* A command that printed nothing leaves an empty screen. That is a
	 * result, not a failure, so it must not be a null buffer. */
	if (!out.data) {
		*lenp = 0;
		return strdup("");
	}
	*lenp = out.len;
	return out.data;

fail:
	free(cells);
	free(out.data);
	return NULL;
}

/* Return the part of the line log that was not read yet. */
static char *tty_log_text(struct fyai_terminal_view *view, size_t from,
			  bool advance, size_t *lenp)
{
	size_t len;
	char *out;

	len = view->log.len > from ? view->log.len - from : 0;
	out = malloc(len + 1);
	fyai_error_check(view->ctx, out, err,
			 "terminal: could not allocate line output");
	if (len)
		memcpy(out, view->log.data + from, len);
	out[len] = '\0';
	if (advance)
		view->log_read = view->log.len;
	*lenp = len;
	return out;

err:
	return NULL;
}

char *fyai_terminal_view_read(struct fyai_terminal_view *view,
			      enum fyai_terminal_read what,
			      const struct fyai_terminal_region *region,
			      size_t *lenp)
{
	int first, last, col, cols;

	if (!view)
		return NULL;
	*lenp = 0;

	switch (what) {
	case FYAITR_REGION:
		if (!region)
			return NULL;
		first = region->row > 0 ? region->row : 0;
		last = region->rows > 0 ? first + region->rows : view->rows;
		col = region->col > 0 ? region->col : 0;
		cols = region->cols > 0 ? region->cols : view->cols - col;
		if (last > view->rows)
			last = view->rows;
		if (col + cols > view->cols)
			cols = view->cols - col;
		if (first >= last || cols <= 0)
			return strdup("");
		return tty_screen_text(view, first, last, col, cols, false,
				       lenp);
	case FYAITR_SCREEN:
		return tty_screen_text(view, 0, view->rows, 0, view->cols,
				       false, lenp);
	case FYAITR_NEW:
		/*
		 * What is new for a program that draws is the picture it
		 * draws; the bytes that made it are cursor movements.
		 */
		if (view->screen_mode)
			return tty_screen_text(view, 0, view->rows, 0,
					       view->cols, false, lenp);
		return tty_log_text(view, view->log_read, true, lenp);
	case FYAITR_ALL:
		if (view->screen_mode)
			return tty_screen_text(view, 0, view->rows, 0,
					       view->cols, true, lenp);
		return tty_log_text(view, 0, false, lenp);
	}
	return NULL;
}

static void tty_cell_color(const struct fyai_terminal_view *view,
			   const union fyvt_color *src, struct fyai_term_color *dst,
			   bool background)
{
	union fyvt_color col = *src;

	dst->is_default = background ? FYVT_COLOR_IS_DEFAULT_BG(&col)
				     : FYVT_COLOR_IS_DEFAULT_FG(&col);
	/* An indexed colour means nothing to another terminal: resolve it. */
	fyvt_screen_convert_color_to_rgb(view->screen, &col);
	dst->r = col.rgb.red;
	dst->g = col.rgb.green;
	dst->b = col.rgb.blue;
}

bool fyai_terminal_view_cell(const struct fyai_terminal_view *view, int row,
			     int col, struct fyai_term_cell *cell)
{
	struct fyvt_screen_cell vc;
	struct fyvt_pos pos;
	int i;

	if (!view || !cell || row < 0 || col < 0 || row >= view->rows ||
	    col >= view->cols)
		return false;

	memset(cell, 0, sizeof(*cell));
	pos.row = row;
	pos.col = col;
	if (!fyvt_screen_get_cell(view->screen, pos, &vc))
		return false;

	for (i = 0; i < FYAI_TERM_CELL_CHARS &&
		    i < FYVT_MAX_CHARS_PER_CELL; i++)
		cell->chars[i] = vc.chars[i];
	cell->width = vc.width;
	cell->bold = !!vc.attrs.bold;
	cell->underline = vc.attrs.underline != FYVT_UNDERLINE_OFF;
	cell->italic = !!vc.attrs.italic;
	cell->blink = !!vc.attrs.blink;
	cell->reverse = !!vc.attrs.reverse;
	cell->strike = !!vc.attrs.strike;
	tty_cell_color(view, &vc.fg, &cell->fg, false);
	tty_cell_color(view, &vc.bg, &cell->bg, true);
	return true;
}

void fyai_terminal_view_cursor(const struct fyai_terminal_view *view,
			       int *rowp, int *colp, bool *visiblep)
{
	if (!view)
		return;
	if (rowp)
		*rowp = view->cursor_row;
	if (colp)
		*colp = view->cursor_col;
	if (visiblep)
		*visiblep = view->cursor_visible;
}

bool fyai_terminal_view_take_damage(struct fyai_terminal_view *view,
				    int *firstp, int *lastp)
{
	if (!view || view->damage_first > view->damage_last) {
		if (view)
			view->cursor_moved = false;
		return false;
	}
	if (firstp)
		*firstp = view->damage_first;
	if (lastp)
		*lastp = view->damage_last;
	view->damage_first = 1;
	view->damage_last = 0;
	view->cursor_moved = false;
	return true;
}

bool fyai_terminal_view_dirty(const struct fyai_terminal_view *view)
{
	return view && (view->damage_first <= view->damage_last ||
			view->cursor_moved);
}

void fyai_terminal_view_damage_all(struct fyai_terminal_view *view)
{
	if (!view)
		return;
	view->damage_first = 0;
	view->damage_last = view->rows - 1;
	view->cursor_moved = true;
}
