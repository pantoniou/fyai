/*
 * fyai_diff.c - line diff of two texts in the unified format
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The common prefix and suffix are removed first; the region between them is
 * compared with the Myers O(ND) algorithm. The trace of each round keeps only
 * the diagonals that the round reached, so the trace of D rounds is D^2
 * entries; past FYAI_DIFF_MAX_EDITS the region is one replacement.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_diff.h"

#define FYAI_DIFF_MAX_EDITS	2000

struct diff_line {
	const char *text;
	size_t len;		/* without the line feed */
	bool eol;		/* ends in a line feed */
};

struct diff_text {
	struct diff_line *lines;
	size_t count;
};

/* One step of the edit script: '=' both, '-' @a alone, '+' @b alone. */
struct diff_ops {
	char *op;
	size_t count;
	size_t cap;
};

static int diff_split(const char *s, size_t len, struct diff_text *t)
{
	const char *end, *nl;
	size_t n;

	t->lines = NULL;
	t->count = 0;
	n = 0;
	for (end = s + len, nl = s; nl < end; nl++)
		if (*nl == '\n')
			n++;
	if (len && s[len - 1] != '\n')
		n++;
	if (!n)
		return 0;
	t->lines = calloc(n, sizeof(*t->lines));
	if (!t->lines)
		return -1;
	while (s < end) {
		nl = memchr(s, '\n', (size_t)(end - s));
		t->lines[t->count].text = s;
		t->lines[t->count].len = nl ? (size_t)(nl - s) :
					       (size_t)(end - s);
		t->lines[t->count].eol = nl != NULL;
		t->count++;
		s = nl ? nl + 1 : end;
	}
	return 0;
}

static bool diff_line_equal(const struct diff_line *x,
			    const struct diff_line *y)
{
	return x->len == y->len && x->eol == y->eol &&
	       !memcmp(x->text, y->text, x->len);
}

static int diff_ops_push(struct diff_ops *ops, char op, size_t times)
{
	char *grown;
	size_t cap;

	if (!times)
		return 0;
	if (ops->count + times > ops->cap) {
		cap = ops->cap ? ops->cap : 256;
		while (cap < ops->count + times)
			cap *= 2;
		grown = realloc(ops->op, cap);
		if (!grown)
			return -1;
		ops->op = grown;
		ops->cap = cap;
	}
	memset(ops->op + ops->count, op, times);
	ops->count += times;
	return 0;
}

/*
 * Append the edit script of @a[0..n) against @b[0..m) to @ops. The script is
 * built backwards from the trace and reversed in place.
 */
static int diff_myers(const struct diff_line *a, long n,
		      const struct diff_line *b, long m, struct diff_ops *ops)
{
	long dmax, d, k, x, y, prev_k, prev_x, prev_y, off, found;
	long *v = NULL, *trace = NULL, *tr;
	size_t start, i;
	char tmp;
	int rc;

	rc = -1;
	dmax = n + m;
	if (dmax > FYAI_DIFF_MAX_EDITS)
		dmax = FYAI_DIFF_MAX_EDITS;
	off = dmax + 1;
	v = calloc((size_t)(2 * dmax + 3), sizeof(*v));
	trace = malloc((size_t)(dmax + 1) * (size_t)(dmax + 1) *
		       sizeof(*trace));
	if (!v || !trace)
		goto out;

	found = -1;
	for (d = 0; d <= dmax && found < 0; d++) {
		for (k = -d; k <= d; k += 2) {
			if (k == -d || (k != d && v[off + k - 1] < v[off + k + 1]))
				x = v[off + k + 1];
			else
				x = v[off + k - 1] + 1;
			y = x - k;
			while (x < n && y < m && diff_line_equal(&a[x], &b[y])) {
				x++;
				y++;
			}
			v[off + k] = x;
			if (x >= n && y >= m) {
				found = d;
				break;
			}
		}
		/* Round d reached the diagonals -d..d. */
		memcpy(trace + d * d, v + off - d, (size_t)(2 * d + 1) *
		       sizeof(*trace));
	}

	if (found < 0) {
		/* Too many edits for a minimal script: replace the region. */
		rc = diff_ops_push(ops, '-', (size_t)n);
		if (!rc)
			rc = diff_ops_push(ops, '+', (size_t)m);
		goto out;
	}

	start = ops->count;
	x = n;
	y = m;
	for (d = found; d > 0; d--) {
		tr = trace + (d - 1) * (d - 1) + (d - 1);	/* index by k */
		k = x - y;
		if (k == -d || (k != d && tr[k - 1] < tr[k + 1]))
			prev_k = k + 1;
		else
			prev_k = k - 1;
		prev_x = tr[prev_k];
		prev_y = prev_x - prev_k;
		while (x > prev_x && y > prev_y) {
			if (diff_ops_push(ops, '=', 1))
				goto out;
			x--;
			y--;
		}
		if (diff_ops_push(ops, prev_k == k + 1 ? '+' : '-', 1))
			goto out;
		if (prev_k == k + 1)
			y--;
		else
			x--;
	}
	while (x > 0 && y > 0) {
		if (diff_ops_push(ops, '=', 1))
			goto out;
		x--;
		y--;
	}
	for (i = 0; i < (ops->count - start) / 2; i++) {
		tmp = ops->op[start + i];
		ops->op[start + i] = ops->op[ops->count - 1 - i];
		ops->op[ops->count - 1 - i] = tmp;
	}
	rc = 0;
out:
	free(trace);
	free(v);
	return rc;
}

static void diff_emit_line(FILE *fp, char mark, const struct diff_line *l)
{
	fputc(mark, fp);
	fwrite(l->text, 1, l->len, fp);
	fputc('\n', fp);
	if (!l->eol)
		fputs("\\ No newline at end of file\n", fp);
}

/* The range of one side of a hunk: a count of 0 names the line before it. */
static void diff_emit_range(FILE *fp, size_t first, size_t count)
{
	if (count == 1)
		fprintf(fp, "%zu", first + 1);
	else
		fprintf(fp, "%zu,%zu", count ? first + 1 : first, count);
}

static int diff_emit_hunks(FILE *fp, const struct diff_ops *ops,
			    const struct diff_text *ta,
			    const struct diff_text *tb, unsigned int context)
{
	size_t i, j, first, last, end, ai, bj, ha, hb, na, nb, a0, b0;
	size_t *apos, *bpos;
	int rc;

	rc = -1;
	apos = malloc((ops->count + 1) * sizeof(*apos));
	bpos = malloc((ops->count + 1) * sizeof(*bpos));
	if (!apos || !bpos)
		goto out;
	ai = bj = 0;
	for (i = 0; i < ops->count; i++) {
		apos[i] = ai;
		bpos[i] = bj;
		if (ops->op[i] != '+')
			ai++;
		if (ops->op[i] != '-')
			bj++;
	}
	apos[i] = ai;
	bpos[i] = bj;

	for (i = 0; i < ops->count; ) {
		if (ops->op[i] == '=') {
			i++;
			continue;
		}
		/* A run of changes joined across short equal gaps. */
		first = i;
		last = i;
		for (j = i + 1; j < ops->count; j++) {
			if (ops->op[j] == '=')
				continue;
			if (j - last - 1 > 2 * (size_t)context)
				break;
			last = j;
		}
		first = first > context ? first - context : 0;
		end = last + 1 + context;
		if (end > ops->count)
			end = ops->count;
		a0 = apos[first];
		b0 = bpos[first];
		na = apos[end] - a0;
		nb = bpos[end] - b0;
		fputs("@@ -", fp);
		diff_emit_range(fp, a0, na);
		fputs(" +", fp);
		diff_emit_range(fp, b0, nb);
		fputs(" @@\n", fp);
		for (ha = a0, hb = b0, j = first; j < end; j++) {
			if (ops->op[j] == '=') {
				diff_emit_line(fp, ' ', &ta->lines[ha++]);
				hb++;
			} else if (ops->op[j] == '-') {
				diff_emit_line(fp, '-', &ta->lines[ha++]);
			} else {
				diff_emit_line(fp, '+', &tb->lines[hb++]);
			}
		}
		i = end;
	}
	rc = 0;
out:
	free(apos);
	free(bpos);
	return rc;
}

int fyai_diff_unified(const char *a, size_t alen, const char *b, size_t blen,
		      const char *aname, const char *bname,
		      unsigned int context, char **outp)
{
	struct diff_text ta = {0}, tb = {0};
	struct diff_ops ops = {0};
	size_t pre, suf, size;
	char *buf = NULL;
	FILE *fp = NULL;
	int rc;

	*outp = NULL;
	rc = -1;
	if (diff_split(a, alen, &ta) || diff_split(b, blen, &tb))
		goto out;

	pre = 0;
	while (pre < ta.count && pre < tb.count &&
	       diff_line_equal(&ta.lines[pre], &tb.lines[pre]))
		pre++;
	suf = 0;
	while (suf < ta.count - pre && suf < tb.count - pre &&
	       diff_line_equal(&ta.lines[ta.count - 1 - suf],
			       &tb.lines[tb.count - 1 - suf]))
		suf++;

	fp = open_memstream(&buf, &size);
	if (!fp)
		goto out;
	if (pre == ta.count && pre == tb.count) {
		rc = 0;
		goto out;
	}
	if (diff_ops_push(&ops, '=', pre) ||
	    diff_myers(ta.lines + pre, (long)(ta.count - pre - suf),
		       tb.lines + pre, (long)(tb.count - pre - suf), &ops) ||
	    diff_ops_push(&ops, '=', suf))
		goto out;
	fprintf(fp, "--- %s\n+++ %s\n", aname, bname);
	if (diff_emit_hunks(fp, &ops, &ta, &tb, context))
		goto out;
	rc = ferror(fp) ? -1 : 0;
out:
	if (fp && fclose(fp))
		rc = -1;
	if (!rc && !buf)
		buf = strdup("");
	if (!rc && buf)
		*outp = buf;
	else
		free(buf);
	if (!rc && !*outp)
		rc = -1;
	free(ops.op);
	free(ta.lines);
	free(tb.lines);
	return rc;
}

/* The file that the walk of fyai_diff_stat() is in. */
struct diff_stat_file {
	char *path;
	size_t added, removed;
	bool open, git, seen_hunk, binary;
};

struct diff_stat {
	struct fy_generic_builder *gb;
	fy_generic *rows;
	size_t count, cap;
	struct diff_stat_file cur;
};

static void diff_stat_reset(struct diff_stat *st)
{
	free(st->cur.path);
	memset(&st->cur, 0, sizeof(st->cur));
}

/* Close the current file into a row; its name may be empty for bare hunks. */
static int diff_stat_close(struct diff_stat *st)
{
	struct diff_stat_file *f = &st->cur;
	fy_generic *rows;
	fy_generic row;

	if (!f->open)
		return 0;
	if (st->count == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 8;
		rows = realloc(st->rows, st->cap * sizeof(*rows));
		if (!rows)
			return -1;
		st->rows = rows;
	}
	row = fy_mapping(st->gb, "path", fy_value(st->gb, f->path ? f->path : ""),
			 "added", (long long)f->added,
			 "removed", (long long)f->removed,
			 "binary", f->binary ? fy_true : fy_false);
	if (!fy_is_valid(row))
		return -1;
	st->rows[st->count++] = row;
	diff_stat_reset(st);
	return 0;
}

static int diff_stat_open(struct diff_stat *st, const char *path, size_t len,
			  bool git)
{
	if (diff_stat_close(st))
		return -1;
	st->cur.open = true;
	st->cur.git = git;
	st->cur.path = strndup(path ? path : "", path ? len : 0);
	return st->cur.path ? 0 : -1;
}

/*
 * The path of `diff --git a/X b/X`: both names are equal, so the middle of
 * the rest is the split, even when the name holds spaces. A rename or a row
 * that does not fit falls back to the first ` b/`.
 */
static void diff_stat_git_path(const char *rest, size_t n, const char **pathp,
			       size_t *lenp)
{
	const char *sep;
	size_t half;

	*pathp = rest;
	*lenp = n;
	if (n < 2 || strncmp(rest, "a/", 2))
		return;
	rest += 2;
	n -= 2;
	if (n >= 3 && (n - 3) % 2 == 0) {
		half = (n - 3) / 2;
		if (!strncmp(rest + half, " b/", 3) &&
		    !strncmp(rest, rest + half + 3, half)) {
			*pathp = rest;
			*lenp = half;
			return;
		}
	}
	sep = memmem(rest, n, " b/", 3);
	*pathp = rest;
	*lenp = sep ? (size_t)(sep - rest) : n;
}

/* Read the lengths of a hunk header; an absent length is one. */
static void diff_stat_hunk(const char *p, size_t n, size_t *oldp, size_t *newp)
{
	char *end;
	const char *q = p + 3, *stop = p + n;
	unsigned long a = 1, b = 1;

	*oldp = *newp = 0;
	if (q >= stop)
		return;
	strtoul(q, &end, 10);
	if (end < stop && *end == ',')
		a = strtoul(end + 1, &end, 10);
	while (end < stop && *end == ' ')
		end++;
	if (end >= stop || *end != '+')
		return;
	strtoul(end + 1, &end, 10);
	if (end < stop && *end == ',')
		b = strtoul(end + 1, &end, 10);
	*oldp = a;
	*newp = b;
}

/* The name on a `---` or `+++` row, without a tab and what follows it. */
static size_t diff_stat_name_len(const char *p, size_t n)
{
	const char *tab = memchr(p, '\t', n);

	return tab ? (size_t)(tab - p) : n;
}

/*
 * Take the name of a `---` or `+++` row. A git file already has its name from
 * the `diff --git` row, and only the new name of a rename replaces it. A
 * name of /dev/null keeps the other side.
 */
static int diff_stat_header(struct diff_stat *st, const char *p, size_t n,
			    bool is_new)
{
	struct diff_stat_file *f = &st->cur;
	size_t len = diff_stat_name_len(p, n);
	char *path;

	if (len == 9 && !strncmp(p, "/dev/null", 9))
		return 0;
	if (f->git) {
		if (!is_new)
			return 0;
		if (len > 2 && !strncmp(p, "b/", 2)) {
			p += 2;
			len -= 2;
		}
	}
	path = strndup(p, len);
	if (!path)
		return -1;
	free(f->path);
	f->path = path;
	return 0;
}

int fyai_diff_stat(struct fy_generic_builder *gb, const char *diff, size_t len,
		   fy_generic *out)
{
	struct diff_stat st = { .gb = gb };
	size_t old_left = 0, new_left = 0, n;
	const char *p = diff, *end = diff + len, *nl, *path;
	int rc = -1;

	*out = fy_invalid;
	while (p < end) {
		nl = memchr(p, '\n', (size_t)(end - p));
		n = nl ? (size_t)(nl - p) : (size_t)(end - p);
		if (old_left || new_left) {
			/* A blank hunk row is a context row that lost its space. */
			switch (n ? *p : ' ') {
			case '+':
				st.cur.added++;
				new_left -= new_left ? 1 : 0;
				break;
			case '-':
				st.cur.removed++;
				old_left -= old_left ? 1 : 0;
				break;
			case '\\':
				break;
			default:
				old_left -= old_left ? 1 : 0;
				new_left -= new_left ? 1 : 0;
				break;
			}
		} else if (n > 11 && !strncmp(p, "diff --git ", 11)) {
			diff_stat_git_path(p + 11, n - 11, &path, &n);
			if (diff_stat_open(&st, path, n, true))
				goto out;
		} else if (n >= 12 && !strncmp(p, "Binary files", 12)) {
			st.cur.open = true;
			st.cur.binary = true;
		} else if (n >= 16 && !strncmp(p, "GIT binary patch", 16)) {
			st.cur.open = true;
			st.cur.binary = true;
		} else if (n >= 4 && !strncmp(p, "--- ", 4)) {
			/* A header without `diff --git` starts a file; with
			 * one, it names the file the git row opened. */
			if (!st.cur.open || (!st.cur.git && st.cur.seen_hunk)) {
				if (diff_stat_open(&st, "", 0, false))
					goto out;
			}
			if (diff_stat_header(&st, p + 4, n - 4, false))
				goto out;
		} else if (n >= 4 && !strncmp(p, "+++ ", 4)) {
			if (!st.cur.open && diff_stat_open(&st, "", 0, false))
				goto out;
			if (diff_stat_header(&st, p + 4, n - 4, true))
				goto out;
		} else if (n >= 3 && !strncmp(p, "@@ ", 3)) {
			if (!st.cur.open && diff_stat_open(&st, "", 0, false))
				goto out;
			st.cur.seen_hunk = true;
			diff_stat_hunk(p, n, &old_left, &new_left);
		}
		p = nl ? nl + 1 : end;
	}
	if (diff_stat_close(&st))
		goto out;
	*out = fy_gb_sequence_create(gb, st.count, st.rows);
	rc = fy_is_valid(*out) ? 0 : -1;
out:
	diff_stat_reset(&st);
	free(st.rows);
	return rc;
}
