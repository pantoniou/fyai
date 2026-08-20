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
	bool screen_mode;
	bool binary;
};

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

/*
 * Append @cells as text, without the blank run at the end of the line. A cell
 * holds a base character and up to FYVT_MAX_CHARS_PER_CELL - 1 combining
 * characters, and the text keeps all of them. A double-width glyph occupies
 * its own cell and leaves the next cell unused. Step over that filler cell,
 * and do not make it a space.
 */
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

/*
 * Keep a retained buffer bounded. A program that does not stop must not grow
 * one without end. Drop the front at a line boundary when the buffer is twice
 * the budget, because a reader keeps the end. Returns the number of bytes
 * dropped, so that a caller can move its marks.
 */
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

/*
 * A row left the top of the screen. Keep it. One screen row is not one line,
 * because the screen stores a long line as several rows, and each row after
 * the first is a continuation. Join them again to keep the result searchable.
 * Write the separating newline before a row that starts a line, and not after
 * the row before it.
 */
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

static const struct fyvt_screen_callbacks tty_screen_callbacks = {
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

/*
 * Decide what a complete escape sequence means. A program that only adds
 * colour to its output still writes lines. A program that redraws one line
 * with a carriage return and an erase also writes lines. A program that moves
 * the cursor, erases the screen, or takes the alternate screen, draws a
 * screen.
 */
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

/*
 * Add one byte to the line log. Colour and other sequences are removed. A
 * carriage return starts the line again, and a backspace removes the last
 * byte, so a progress line keeps only its last state.
 */
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
	 * thus a carriage return is only an overwrite when another character
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

struct fyai_terminal_view *fyai_terminal_view_create(int rows, int cols,
						     size_t max_bytes)
{
	struct fyai_terminal_view *view;
	struct fyvt_cfg vtcfg;

	view = calloc(1, sizeof(*view));
	if (!view)
		return NULL;

	view->rows = rows > 0 ? rows : FYAI_TTY_ROWS_DEFAULT;
	view->cols = cols > 0 ? cols : FYAI_TTY_COLS_DEFAULT;
	view->max_bytes = max_bytes;
	view->scan = TTYSC_TEXT;

	fyvt_cfg_default(&vtcfg);
	vtcfg.rows = view->rows;
	vtcfg.cols = view->cols;
	view->vt = fyvt_create(&vtcfg);
	if (!view->vt) {
		free(view);
		return NULL;
	}
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
	fyvt_screen_reset(view->screen, 1);
	return view;
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
	size_t i;

	if (!view || !data || !len)
		return 0;

	if (!view->binary && data_is_binary(data, len))
		view->binary = true;
	view->raw_bytes += len;

	/* The screen is interpreted from the same bytes as the line log. */
	(void)fyvt_input_write(view->vt, data, len);
	for (i = 0; i < len; i++)
		tty_log_feed(view, (unsigned char)data[i]);
	return 0;
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
	int row, i;

	cells = calloc((size_t)view->cols, sizeof(*cells));
	if (!cells)
		return NULL;

	if (with_sb && view->sb.len &&
	    response_buffer_append_data(&out, view->sb.data, view->sb.len))
		goto fail;

	for (row = first; row < last; row++) {
		if (out.len && !tty_row_continues(view, row) &&
		    response_buffer_append_data(&out, "\n", 1))
			goto fail;
		for (i = 0; i < cols; i++) {
			pos.row = row;
			pos.col = col + i;
			if (!fyvt_screen_get_cell(view->screen, pos,
						   &cells[i]))
				memset(&cells[i], 0, sizeof(cells[i]));
		}
		if (tty_append_cells(&out, cells, cols))
			goto fail;
	}
	free(cells);

	/* A screen is mostly blank at the bottom; do not send empty rows. */
	response_buffer_trim(&out);
	/* A command that printed nothing leaves an empty screen. That is a
	 * result, not a failure, thus it must not be a null buffer. */
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
	if (!out)
		return NULL;
	if (len)
		memcpy(out, view->log.data + from, len);
	out[len] = '\0';
	if (advance)
		view->log_read = view->log.len;
	*lenp = len;
	return out;
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
