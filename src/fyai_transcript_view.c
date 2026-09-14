/* SPDX-License-Identifier: MIT */
/*
 * fyai_transcript_view.c - the transcript of a fullscreen page.
 *
 * The stored conversation is rendered through the display path of a view
 * context, as the branch browser renders its preview, so the view is not a
 * second transcript renderer. It is rendered one exchange at a time, and the
 * rows of an exchange are kept with its key and the width they were made at,
 * so a new turn renders only its exchange.
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <limits.h>
#include <libfymd4c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_display.h"
#include "fyai_flow.h"
#include "fyai_markdown.h"
#include "fyai_sink.h"
#include "fyai_transcript_view.h"
#include "fyai_turn.h"

/* Rows: owned strings, without their newlines. */
struct view_rows {
	char **row;
	size_t count;
	size_t alloc;
};

/* One stored exchange at @width for @key: its @count rows, which @rows holds
 * when it is @rendered and a measure stands for when it is not. */
struct view_exchange {
	uintptr_t key;
	int width;
	size_t count;
	bool rendered;
	struct view_rows rows;
};

struct fyai_transcript_view {
	struct view_exchange *exchange;
	size_t nexchanges;
	size_t stored;		/* the rows of every exchange */
	struct view_rows live;
	bool live_open;		/* the last live row did not end yet */
	int offset;		/* rows back from the end of the transcript */
	int height;		/* the rows of the last window */
	const char **window;
	size_t window_alloc;
	/* What the stored rows were brought up to. */
	bool rendered;
	fy_generic head;
	int width;
};

static void view_rows_clear(struct view_rows *r)
{
	size_t i;

	for (i = 0; i < r->count; i++)
		free(r->row[i]);
	r->count = 0;
}

static void view_rows_free(struct view_rows *r)
{
	view_rows_clear(r);
	free(r->row);
	r->row = NULL;
	r->alloc = 0;
}

static int view_rows_push(struct view_rows *r, const char *text, size_t len)
{
	char **row;
	size_t alloc;

	if (r->count == r->alloc) {
		alloc = r->alloc ? r->alloc * 2 : 64;
		row = realloc(r->row, alloc * sizeof(*row));
		if (!row)
			return -1;
		r->row = row;
		r->alloc = alloc;
	}
	r->row[r->count] = strndup(text, len);
	if (!r->row[r->count])
		return -1;
	r->count++;
	return 0;
}

/* Continue the last row with @len bytes of @text. */
static int view_rows_extend(struct view_rows *r, const char *text, size_t len)
{
	char *row;
	size_t have;

	if (!r->count)
		return view_rows_push(r, text, len);
	have = strlen(r->row[r->count - 1]);
	row = realloc(r->row[r->count - 1], have + len + 1);
	if (!row)
		return -1;
	memcpy(row + have, text, len);
	row[have + len] = '\0';
	r->row[r->count - 1] = row;
	return 0;
}

/*
 * Append the lines of @text to @r. The first line continues the last row when
 * *@openp; *@openp says whether the last line ended without a newline.
 * *@addedp receives the rows added.
 */
static int view_rows_append(struct view_rows *r, const char *text, size_t len,
			    bool *openp, size_t *addedp)
{
	const char *p = text, *end = text + len, *nl;
	size_t n;
	int rc;

	*addedp = 0;
	while (p < end) {
		nl = memchr(p, '\n', (size_t)(end - p));
		n = nl ? (size_t)(nl - p) : (size_t)(end - p);
		if (*openp) {
			rc = view_rows_extend(r, p, n);
		} else {
			rc = view_rows_push(r, p, n);
			if (!rc)
				(*addedp)++;
		}
		if (rc)
			return -1;
		*openp = !nl;
		p = nl ? nl + 1 : end;
	}
	return 0;
}

/* The rows of @text, which is one exchange. */
static int view_rows_parse(struct view_rows *r, const char *text, size_t len)
{
	size_t added;
	bool open = false;

	memset(r, 0, sizeof(*r));
	if (len && view_rows_append(r, text, len, &open, &added)) {
		view_rows_free(r);
		return -1;
	}
	return 0;
}

static void view_exchanges_free(struct view_exchange *x, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		view_rows_free(&x[i].rows);
	free(x);
}

/* The stored row @index of @x: a row of an exchange only measured is blank. */
static const char *view_stored_row(const struct view_exchange *x, size_t n,
				   size_t index)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (index < x[i].count)
			return x[i].rendered && index < x[i].rows.count ?
			       x[i].rows.row[index] : "";
		index -= x[i].count;
	}
	return "";
}

/* The first stored row a region of @height rows shows, and how many it shows,
 * over @stored stored rows, @live live rows and @offset rows back. */
static void view_span(size_t stored, size_t live, int offset, int height,
		      size_t *firstp, size_t *shownp)
{
	size_t total = stored + live, shown, back;

	shown = height > 0 && total > (size_t)height ? (size_t)height : total;
	back = offset > 0 ? (size_t)offset : 0;
	if (back > total - shown)
		back = total - shown;
	*firstp = total - shown - back;
	*shownp = shown;
}

/* A growing string: always NUL terminated once it holds anything. */
struct view_text {
	char *data;
	size_t len;
	size_t alloc;
};

static int view_text_append(struct view_text *t, const char *s, size_t n)
{
	char *data;
	size_t alloc;

	if (t->len + n + 1 > t->alloc) {
		alloc = t->alloc ? t->alloc : 128;
		while (alloc < t->len + n + 1)
			alloc *= 2;
		data = realloc(t->data, alloc);
		if (!data)
			return -1;
		t->data = data;
		t->alloc = alloc;
	}
	memcpy(t->data + t->len, s, n);
	t->len += n;
	t->data[t->len] = '\0';
	return 0;
}

/* The bytes of the escape sequence at @p, which ends before @end. */
static size_t view_escape_len(const char *p, const char *end)
{
	const char *q;

	if (end - p < 2)
		return (size_t)(end - p);
	if (p[1] == '[') {
		for (q = p + 2; q < end; q++)
			if (*q >= 0x40 && *q <= 0x7e)
				return (size_t)(q + 1 - p);
		return (size_t)(end - p);
	}
	if (p[1] == ']') {
		for (q = p + 2; q < end; q++) {
			if (*q == '\a')
				return (size_t)(q + 1 - p);
			if (*q == '\x1b' && q + 1 < end && q[1] == '\\')
				return (size_t)(q + 2 - p);
		}
		return (size_t)(end - p);
	}
	return 2;
}

/*
 * Append to @t the text of @line that stands in the columns @from to @to,
 * inclusive. A mark that has no width goes with the character before it.
 */
static int view_line_columns(struct view_text *t, const char *line, int from,
			     int to)
{
	const char *p = line, *end = line + strlen(line);
	unsigned int cp;
	size_t n;
	int col = 0, width;
	bool took = false;

	while (p < end) {
		if (*p == '\x1b') {
			p += view_escape_len(p, end);
			continue;
		}
		n = fymd_utf8_decode(p, (size_t)(end - p), &cp);
		if (!n)
			break;
		width = fymd_cp_width(cp);
		if (width == 0 ? took : (col >= from && col <= to)) {
			if (view_text_append(t, p, n))
				return -1;
			took = true;
		} else if (width) {
			took = false;
		}
		col += width;
		p += n;
	}
	return 0;
}

char *fyai_transcript_view_copy(struct fyai_transcript_view *v, int height,
				int row0, int col0, int row1, int col1)
{
	struct view_text t = {0};
	const char *const *window;
	const char *line;
	size_t start;
	int count = 0, row, swap;

	if (!v)
		return NULL;
	if (row1 < row0 || (row1 == row0 && col1 < col0)) {
		swap = row0; row0 = row1; row1 = swap;
		swap = col0; col0 = col1; col1 = swap;
	}
	window = fyai_transcript_view_window(v, height, &count);
	for (row = row0; row <= row1; row++) {
		if (row > row0 && view_text_append(&t, "\n", 1))
			goto fail;
		start = t.len;
		line = window && row >= 0 && row < count ? window[row] : "";
		if (view_line_columns(&t, line, row == row0 ? col0 : 0,
				      row == row1 ? col1 : INT_MAX))
			goto fail;
		while (t.len > start && t.data[t.len - 1] == ' ')
			t.data[--t.len] = '\0';
	}
	if (!t.data)
		return strdup("");
	return t.data;
fail:
	free(t.data);
	return NULL;
}

struct fyai_transcript_view *fyai_transcript_view_create(void)
{
	struct fyai_transcript_view *v = calloc(1, sizeof(*v));

	if (v)
		v->head = fy_invalid;
	return v;
}

void fyai_transcript_view_destroy(struct fyai_transcript_view *v)
{
	if (!v)
		return;
	view_exchanges_free(v->exchange, v->nexchanges);
	view_rows_free(&v->live);
	free(v->window);
	free(v);
}

size_t fyai_transcript_view_rows(const struct fyai_transcript_view *v)
{
	return v ? v->stored + v->live.count : 0;
}

bool fyai_transcript_view_at_end(const struct fyai_transcript_view *v)
{
	return !v || v->offset == 0;
}

int fyai_transcript_view_set_stored(struct fyai_transcript_view *v,
				    const char *text, size_t len)
{
	struct view_exchange *x;

	if (!v || (!text && len))
		return -1;
	x = calloc(1, sizeof(*x));
	if (!x)
		return -1;
	if (view_rows_parse(&x->rows, text, len)) {
		free(x);
		return -1;
	}
	x->count = x->rows.count;
	x->rendered = true;
	view_exchanges_free(v->exchange, v->nexchanges);
	v->exchange = x;
	v->nexchanges = 1;
	v->stored = x->count;
	return 0;
}

/* The index of the row at the top of the view, when it is scrolled back. */
static bool view_top(const struct fyai_transcript_view *v, int height,
		     size_t *topp)
{
	size_t first, shown;

	if (v->offset <= 0 || height < 1)
		return false;
	view_span(v->stored, v->live.count, v->offset, height, &first, &shown);
	*topp = first;
	return true;
}

/*
 * The offset that puts row @row of exchange @at of @x at the top of a region of
 * @height rows, over @live live rows.
 */
static int view_anchor_offset(const struct view_exchange *x, size_t n,
			      size_t at, size_t row, size_t live, int height)
{
	size_t stored = 0, start = 0, first, shown, top, i;

	for (i = 0; i < n; i++) {
		if (i < at)
			start += x[i].count;
		stored += x[i].count;
	}
	if (at < n && row >= x[at].count)
		row = x[at].count ? x[at].count - 1 : 0;
	top = start + row;
	view_span(stored, live, 0, height, &first, &shown);
	return first > top ? (int)(first - top) : 0;
}

/* Render @x, which exchange @index is, in place of its measure. */
static int view_render_one(struct view_exchange *x, size_t index, int width,
			   fyai_transcript_view_render_fn render, void *user)
{
	char *text = NULL;
	size_t len = 0;
	int rc;

	rc = render(user, index, width, &text, &len);
	if (!rc)
		rc = view_rows_parse(&x->rows, text ? text : "", text ? len : 0);
	free(text);
	if (rc)
		return -1;
	x->count = x->rows.count;
	x->rendered = true;
	return 0;
}

bool fyai_transcript_view_needs_render(const struct fyai_transcript_view *v,
				       int height)
{
	size_t first, shown, start = 0, i;

	if (!v || height < 1)
		return false;
	view_span(v->stored, v->live.count, v->offset, height, &first, &shown);
	for (i = 0; i < v->nexchanges && start < first + shown; i++) {
		if (!v->exchange[i].rendered && start + v->exchange[i].count > first)
			return true;
		start += v->exchange[i].count;
	}
	return false;
}

int fyai_transcript_view_update(struct fyai_transcript_view *v, int width,
				int height, const uintptr_t *keys,
				size_t count,
				fyai_transcript_view_render_fn render,
				fyai_transcript_view_measure_fn measure,
				void *user)
{
	struct view_exchange *next = NULL, *old, *x;
	size_t i, j, k, hint = 0, top = 0, stored, start, first, shown;
	size_t anchor_at = SIZE_MAX;
	uintptr_t anchor_key = 0;
	size_t anchor_row = 0;
	bool anchored = false, again;
	int offset = v ? v->offset : 0, pass;

	if (!v || (count && (!keys || !render)))
		return -1;
	if (height < 1)
		height = v->height;
	/* The row at the top of a view scrolled back: its exchange and row. */
	if (view_top(v, height, &top) && top < v->stored) {
		for (i = 0; i < v->nexchanges; i++) {
			if (top < v->exchange[i].count) {
				anchor_key = v->exchange[i].key;
				anchor_row = top;
				anchored = true;
				break;
			}
			top -= v->exchange[i].count;
		}
	}
	if (count) {
		next = calloc(count, sizeof(*next));
		if (!next)
			return -1;
	}
	for (i = 0; i < count; i++) {
		next[i].key = keys[i];
		next[i].width = width;
		/* An exchange that did not change keeps its rows or its
		 * measure: they are most often where they were. */
		old = NULL;
		for (j = 0; j < v->nexchanges; j++) {
			x = &v->exchange[(hint + j) % v->nexchanges];
			if (x->key == keys[i] && x->width == width &&
			    x->count != SIZE_MAX) {
				old = x;
				hint = (hint + j + 1) % v->nexchanges;
				break;
			}
		}
		if (old) {
			next[i].rows = old->rows;
			next[i].count = old->count;
			next[i].rendered = old->rendered;
			memset(&old->rows, 0, sizeof(old->rows));
			old->count = SIZE_MAX;	/* taken */
			continue;
		}
		if (measure) {
			next[i].count = measure(user, i, width);
			continue;
		}
		if (view_render_one(&next[i], i, width, render, user))
			goto fail;
	}
	stored = 0;
	for (i = 0; i < count; i++)
		stored += next[i].count;
	/* The view goes back to the row it showed at its top. */
	for (i = 0; anchored && i < count; i++)
		if (next[i].key == anchor_key) {
			anchor_at = i;
			break;
		}
	if (anchor_at != SIZE_MAX)
		offset = view_anchor_offset(next, count, anchor_at, anchor_row,
					    v->live.count, height);
	/* Render what the region shows. A render can change the rows of an
	 * exchange from its measure, and so what the region shows. */
	for (pass = 0; measure && height > 0 && pass < 4; pass++) {
		view_span(stored, v->live.count, offset, height, &first, &shown);
		again = false;
		for (i = 0, start = 0; i < count && start < first + shown; i++) {
			if (!next[i].rendered && start + next[i].count > first) {
				stored -= next[i].count;
				if (view_render_one(&next[i], i, width, render,
						    user))
					goto fail;
				stored += next[i].count;
				again = true;
			}
			start += next[i].count;
		}
		if (!again)
			break;
		/* A render that changed the rows of an exchange moved the row
		 * the view keeps at its top. */
		if (anchor_at != SIZE_MAX)
			offset = view_anchor_offset(next, count, anchor_at,
						    anchor_row, v->live.count,
						    height);
	}
	view_exchanges_free(v->exchange, v->nexchanges);
	v->exchange = next;
	v->nexchanges = count;
	v->stored = stored;
	v->width = width;
	v->height = height;
	v->offset = offset;
	return 0;

fail:
	/* The kept rows go back where they came from. */
	for (j = 0; j < count; j++)
		for (k = 0; k < v->nexchanges; k++)
			if (v->exchange[k].count == SIZE_MAX &&
			    v->exchange[k].key == next[j].key &&
			    v->exchange[k].width == width) {
				v->exchange[k].rows = next[j].rows;
				v->exchange[k].count = next[j].count;
				v->exchange[k].rendered = next[j].rendered;
				memset(&next[j].rows, 0, sizeof(next[j].rows));
				break;
			}
	view_exchanges_free(next, count);
	return -1;
}

int fyai_transcript_view_append_live(struct fyai_transcript_view *v,
				     const char *text, size_t len)
{
	size_t added = 0;
	int rc;

	if (!v || (!text && len))
		return -1;
	if (!len)
		return 0;
	rc = view_rows_append(&v->live, text, len, &v->live_open, &added);
	/* A view scrolled back keeps its top row as rows arrive. */
	if (v->offset > 0 && added <= (size_t)(INT_MAX - v->offset))
		v->offset += (int)added;
	return rc;
}

void fyai_transcript_view_clear_live(struct fyai_transcript_view *v)
{
	if (!v)
		return;
	view_rows_clear(&v->live);
	v->live_open = false;
}

void fyai_transcript_view_scroll(struct fyai_transcript_view *v, int delta,
				 int height)
{
	size_t total;
	long long offset, max;

	if (!v)
		return;
	total = fyai_transcript_view_rows(v);
	max = height > 0 && total > (size_t)height ? (long long)total - height : 0;
	offset = (long long)v->offset + delta;
	if (offset > max)
		offset = max;
	if (offset < 0)
		offset = 0;
	v->offset = (int)offset;
}

const char *const *fyai_transcript_view_window(struct fyai_transcript_view *v,
					       int height, int *countp)
{
	const char **window;
	size_t first, n, i, e, in;

	if (countp)
		*countp = 0;
	if (!v || height < 1)
		return NULL;
	v->height = height;
	/* The rows that arrived may have left an offset past the start. */
	fyai_transcript_view_scroll(v, 0, height);
	view_span(v->stored, v->live.count, v->offset, height, &first, &n);
	if ((size_t)height > v->window_alloc) {
		window = realloc(v->window, (size_t)height * sizeof(*window));
		if (!window)
			return NULL;
		v->window = window;
		v->window_alloc = (size_t)height;
	}
	/* Walk the exchanges once to the first row, then row by row. */
	e = 0;
	in = first;
	while (e < v->nexchanges && in >= v->exchange[e].count) {
		in -= v->exchange[e].count;
		e++;
	}
	for (i = 0; i < n; i++) {
		if (first + i >= v->stored) {
			v->window[i] = v->live.row[first + i - v->stored];
			continue;
		}
		while (e < v->nexchanges && in >= v->exchange[e].count) {
			in = 0;
			e++;
		}
		if (e >= v->nexchanges) {
			v->window[i] = view_stored_row(v->exchange,
						       v->nexchanges, first + i);
			continue;
		}
		v->window[i] = v->exchange[e].rendered &&
			       in < v->exchange[e].rows.count ?
			       v->exchange[e].rows.row[in] : "";
		in++;
	}
	if (countp)
		*countp = (int)n;
	return (const char *const *)v->window;
}

/* What the render and the measure of one stored exchange need. */
struct view_render {
	struct fyai_ctx *ctx;
	struct fyai_turn_stack *stack;
	size_t *starts;
	struct fymd_renderer *measurer;	/* made on the first measure */
	int measurer_width;
};

static size_t view_measure_exchange(void *user, size_t index, int width)
{
	struct view_render *r = user;

	if (r->measurer && r->measurer_width != width) {
		fymd_renderer_destroy(r->measurer);
		r->measurer = NULL;
	}
	if (!r->measurer) {
		r->measurer = markdown_measurer_create(r->ctx->cfg,
						       (size_t)width);
		r->measurer_width = width;
	}
	/* Without a measurer the exchange takes a row until it renders. */
	if (!r->measurer)
		return 1;
	return fyai_display_turn_range_rows(r->ctx, r->measurer, r->stack,
					    r->starts[index],
					    r->starts[index + 1]);
}

static int view_render_exchange(void *user, size_t index, int width,
				char **textp, size_t *lenp)
{
	struct fy_generic_builder_cfg gbcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct view_render *r = user;
	struct fyai_ctx *ctx = r->ctx;
	struct fy_generic_builder *gb;
	struct fyai_ctx view;
	struct fyai_cfg cfg;
	FILE *fp = NULL;
	int rc = -1;

	*textp = NULL;
	*lenp = 0;
	view = *ctx;
	cfg = *ctx->cfg;
	gb = fy_generic_builder_create(&gbcfg);
	fyai_error_check(ctx, gb, out, "cannot build the transcript view");
	view.cfg = &cfg;
	view.ui = NULL;
	view.display_output = NULL;
	view.browser = NULL;
	view.transient_gb = gb;
	cfg.render_width = width;
	fp = open_memstream(textp, lenp);
	fyai_error_check(ctx, fp, out, "cannot render the transcript view");
	view.sink = fyai_sink_create_render(&view, fp);
	fyai_error_check(ctx, view.sink, out,
			 "cannot render the transcript view");
	/* An exchange after another is separated from it as a replay
	 * separates them. */
	if (index)
		fyai_flow_emitted(fyai_sink_flow(view.sink), FYAI_FLOW_PROSE,
				  true);
	if (fyai_display_turn_range(&view, r->stack, r->starts[index],
				    r->starts[index + 1], index > 0))
		fyai_warning(ctx, "the transcript view is incomplete");
	fyai_sink_destroy(view.sink);
	rc = 0;
out:
	/* The memstream must be closed before *@textp names its bytes. */
	if (fp && fclose(fp)) {
		fyai_warning(ctx, "the transcript view was not completed");
		rc = -1;
	}
	if (rc) {
		free(*textp);
		*textp = NULL;
		*lenp = 0;
	}
	if (gb)
		fy_generic_builder_destroy(gb);
	return rc;
}

int fyai_transcript_view_refresh(struct fyai_ctx *ctx,
				 struct fyai_transcript_view *v, int width,
				 int height)
{
	struct fyai_turn_stack stack;
	struct view_render r;
	size_t *starts = NULL;
	uintptr_t *keys = NULL;
	size_t count, exchanges = 0, i;
	bool head_changed;
	int rc = -1;

	if (!ctx || !v || width < 1)
		return 0;
	head_changed = !v->rendered || v->head.v != ctx->last_message.v;
	/* A region scrolled onto an exchange that is only measured renders
	 * it. */
	if (!head_changed && v->width == width &&
	    !fyai_transcript_view_needs_render(v, height))
		return 0;
	memset(&stack, 0, sizeof(stack));
	rc = fyai_turn_stack_init(&stack, ctx->last_message, fy_invalid);
	fyai_error_check(ctx, !rc, out, "could not read the conversation");
	rc = -1;
	count = stack.count;
	starts = calloc(count + 1, sizeof(*starts));
	keys = calloc(count + 1, sizeof(*keys));
	fyai_error_check(ctx, starts && keys, out,
			 "cannot list the exchanges of the transcript view");
	for (i = 0; i < count; i++)
		if (fyai_turn_has_user_message(stack.items[i]))
			starts[exchanges++] = i;
	/* The turns before the first question go with it, as a recap has it. */
	if (exchanges)
		starts[0] = 0;
	starts[exchanges] = count;
	/* An exchange is the turns it stored: its last turn names it. */
	for (i = 0; i < exchanges; i++)
		keys[i] = (uintptr_t)stack.items[starts[i + 1] - 1].v;
	r.ctx = ctx;
	r.stack = &stack;
	r.starts = starts;
	r.measurer = NULL;
	r.measurer_width = 0;
	rc = fyai_transcript_view_update(v, width, height, keys, exchanges,
					 view_render_exchange,
					 view_measure_exchange, &r);
	fymd_renderer_destroy(r.measurer);
	fyai_error_check(ctx, !rc, out,
			 "cannot keep the rows of the transcript view");
	if (head_changed)
		fyai_transcript_view_clear_live(v);
	v->rendered = true;
	v->head = ctx->last_message;
out:
	free(keys);
	free(starts);
	fyai_turn_stack_cleanup(&stack);
	return rc;
}
