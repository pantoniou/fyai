/*
 * fyai_patch.c - Codex-style patch tool implementation
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_patch.h"
#include "utils.h"

struct patch_line {
	const char *p;
	size_t len;
};

struct patch_reader {
	const char *p;
};

enum patch_op_kind {
	PATCH_OP_ADD,
	PATCH_OP_DELETE,
	PATCH_OP_UPDATE,
};

struct patch_op {
	enum patch_op_kind kind;
	char *path;
	char *new_path;
	char *content;
	struct patch_op *next;
};

/* A growable array of owned (malloc'd) C strings. */
struct str_list {
	char **item;
	size_t count;
	size_t cap;
};

/*
 * One Codex "@@"-delimited chunk of an Update File hunk: an optional
 * single-line change_context to narrow the search window, followed by the
 * old/new line blocks. Mirrors codex-rs apply-patch's UpdateFileChunk.
 */
struct update_chunk {
	char *context;
	struct str_list old;
	struct str_list new;
	bool is_eof;
};

struct update_chunk_list {
	struct update_chunk *item;
	size_t count;
	size_t cap;
};

/* A single (start_index, old_len, new_lines) splice, as codex computes it. */
struct replacement {
	size_t idx;
	size_t old_len;
	char **new_item;
	size_t new_count;
};

static bool patch_next_line(struct patch_reader *r, struct patch_line *line)
{
	const char *nl;

	if (!*r->p)
		return false;

	line->p = r->p;
	nl = strchr(r->p, '\n');
	if (nl) {
		line->len = (size_t)(nl - r->p);
		r->p = nl + 1;
	} else {
		line->len = strlen(r->p);
		r->p += line->len;
	}
	if (line->len && line->p[line->len - 1] == '\r')
		line->len--;
	return true;
}

static bool line_starts(struct patch_line *line, const char *s)
{
	size_t len;

	len = strlen(s);
	return line->len >= len && !memcmp(line->p, s, len);
}

static bool str_starts(const char *s, const char *pfx)
{
	size_t len;

	len = strlen(pfx);
	return !strncmp(s, pfx, len);
}

static char *line_dup_offset(struct patch_line *line, size_t off)
{
	char *out;
	size_t len;

	len = line->len - off;
	out = malloc(len + 1);
	if (!out)
		return NULL;
	memcpy(out, line->p + off, len);
	out[len] = '\0';
	return out;
}

static bool is_ascii_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == '\f' || c == '\v';
}

/*
 * Trim ASCII whitespace from both ends of a patch line and return a fresh
 * NUL-terminated copy. Used only for marker-line matching (Begin/End Patch,
 * "*** ..." headers, "@@" context lines): codex-rs's parser tolerates
 * leading/trailing whitespace padding around these lines.
 */
static char *patch_line_trim_dup(struct patch_line *line)
{
	size_t start, end;
	char *out;

	start = 0;
	end = line->len;
	while (start < end && is_ascii_space((unsigned char)line->p[start]))
		start++;
	while (end > start && is_ascii_space((unsigned char)line->p[end - 1]))
		end--;
	out = malloc(end - start + 1);
	if (!out)
		return NULL;
	memcpy(out, line->p + start, end - start);
	out[end - start] = '\0';
	return out;
}

static char *rstrip_dup(const char *s)
{
	size_t len;

	len = strlen(s);
	while (len && is_ascii_space((unsigned char)s[len - 1]))
		len--;
	return strndup(s, len);
}

static char *trim_dup(const char *s)
{
	size_t start, end;

	start = 0;
	end = strlen(s);
	while (start < end && is_ascii_space((unsigned char)s[start]))
		start++;
	while (end > start && is_ascii_space((unsigned char)s[end - 1]))
		end--;
	return strndup(s + start, end - start);
}

/*
 * Fold a handful of "typographic" Unicode code points (dashes, curly
 * quotes, exotic spaces) down to their ASCII equivalents, mirroring
 * codex-rs's seek_sequence::normalise(). This is the last, most permissive
 * matching tier, so a diff authored in plain ASCII can still find context
 * that a formatter turned into em-dashes/smart quotes.
 */
static const struct {
	unsigned char seq[3];
	size_t len;
	char repl;
} uni_fold[] = {
	{ { 0xE2, 0x80, 0x90 }, 3, '-' },	/* U+2010 hyphen */
	{ { 0xE2, 0x80, 0x91 }, 3, '-' },	/* U+2011 nb-hyphen */
	{ { 0xE2, 0x80, 0x92 }, 3, '-' },	/* U+2012 figure dash */
	{ { 0xE2, 0x80, 0x93 }, 3, '-' },	/* U+2013 en dash */
	{ { 0xE2, 0x80, 0x94 }, 3, '-' },	/* U+2014 em dash */
	{ { 0xE2, 0x80, 0x95 }, 3, '-' },	/* U+2015 horizontal bar */
	{ { 0xE2, 0x88, 0x92 }, 3, '-' },	/* U+2212 minus sign */
	{ { 0xE2, 0x80, 0x98 }, 3, '\'' },	/* U+2018 */
	{ { 0xE2, 0x80, 0x99 }, 3, '\'' },	/* U+2019 */
	{ { 0xE2, 0x80, 0x9A }, 3, '\'' },	/* U+201A */
	{ { 0xE2, 0x80, 0x9B }, 3, '\'' },	/* U+201B */
	{ { 0xE2, 0x80, 0x9C }, 3, '"' },	/* U+201C */
	{ { 0xE2, 0x80, 0x9D }, 3, '"' },	/* U+201D */
	{ { 0xE2, 0x80, 0x9E }, 3, '"' },	/* U+201E */
	{ { 0xE2, 0x80, 0x9F }, 3, '"' },	/* U+201F */
	{ { 0xC2, 0xA0, 0x00 }, 2, ' ' },	/* U+00A0 nbsp */
	{ { 0xE2, 0x80, 0x82 }, 3, ' ' },	/* U+2002 */
	{ { 0xE2, 0x80, 0x83 }, 3, ' ' },	/* U+2003 */
	{ { 0xE2, 0x80, 0x84 }, 3, ' ' },	/* U+2004 */
	{ { 0xE2, 0x80, 0x85 }, 3, ' ' },	/* U+2005 */
	{ { 0xE2, 0x80, 0x86 }, 3, ' ' },	/* U+2006 */
	{ { 0xE2, 0x80, 0x87 }, 3, ' ' },	/* U+2007 */
	{ { 0xE2, 0x80, 0x88 }, 3, ' ' },	/* U+2008 */
	{ { 0xE2, 0x80, 0x89 }, 3, ' ' },	/* U+2009 */
	{ { 0xE2, 0x80, 0x8A }, 3, ' ' },	/* U+200A */
	{ { 0xE2, 0x80, 0xAF }, 3, ' ' },	/* U+202F */
	{ { 0xE2, 0x81, 0x9F }, 3, ' ' },	/* U+205F */
	{ { 0xE3, 0x80, 0x80 }, 3, ' ' },	/* U+3000 */
};

static char *normalize_dup(const char *s)
{
	struct response_buffer out = {};
	char *trimmed;
	const unsigned char *p;
	size_t i, n, k;
	bool matched;

	trimmed = trim_dup(s);
	if (!trimmed)
		return NULL;
	p = (const unsigned char *)trimmed;
	n = strlen(trimmed);
	for (i = 0; i < n;) {
		matched = false;
		for (k = 0; k < ARRAY_SIZE(uni_fold); k++) {
			if (i + uni_fold[k].len <= n &&
			    !memcmp(p + i, uni_fold[k].seq, uni_fold[k].len)) {
				if (response_buffer_reserve(&out, out.len + 2)) {
					free(trimmed);
					free(out.data);
					return NULL;
				}
				out.data[out.len++] = uni_fold[k].repl;
				out.data[out.len] = '\0';
				i += uni_fold[k].len;
				matched = true;
				break;
			}
		}
		if (matched)
			continue;
		if (response_buffer_reserve(&out, out.len + 2)) {
			free(trimmed);
			free(out.data);
			return NULL;
		}
		out.data[out.len++] = (char)p[i];
		out.data[out.len] = '\0';
		i++;
	}
	free(trimmed);
	return out.data ? out.data : strdup("");
}

static bool patch_path_ok(const char *path)
{
	const char *p;

	if (!path || !*path || path[0] == '/')
		return false;
	if (strstr(path, "//"))
		return false;

	p = path;
	while (*p) {
		if ((p[0] == '.' && p[1] == '.' &&
		     (p[2] == '/' || p[2] == '\0')) ||
		    (p[0] == '/' && p[1] == '.' && p[2] == '.' &&
		     (p[3] == '/' || p[3] == '\0')))
			return false;
		p++;
	}
	return true;
}

static char *patch_err(const char *msg)
{
	char *out;

	if (asprintf(&out, "tool error: %s", msg) < 0)
		return NULL;
	return out;
}

static char *patch_errf(const char *fmt, const char *arg)
{
	char *out;

	if (asprintf(&out, fmt, arg) < 0)
		return NULL;
	return out;
}

static char *patch_errno(const char *op, const char *path)
{
	char *out;

	if (asprintf(&out, "tool error: %s %s: %s", op, path,
		     strerror(errno)) < 0)
		return NULL;
	return out;
}

static int patch_mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	char *p;
	size_t len;

	len = strlen(path);
	if (!len || len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0700) && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(tmp, 0700) && errno != EEXIST ? -1 : 0;
}

static int patch_ensure_parent_dir(const char *path)
{
	char tmp[PATH_MAX];
	char *slash;
	size_t len;

	len = strlen(path);
	if (len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);
	slash = strrchr(tmp, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return patch_mkdir_p(tmp);
}

static int write_text_file_atomic(const char *path, const char *content)
{
	char *tmp;
	FILE *fp;
	int fd;
	int rc;

	fp = NULL;
	fd = -1;
	rc = -1;
	if (asprintf(&tmp, "%s.fyai-patch-XXXXXX", path) < 0)
		return -1;

	fd = mkstemp(tmp);
	if (fd < 0)
		goto out;
	fp = fdopen(fd, "wb");
	if (!fp)
		goto out;
	fd = -1;
	if (fputs(content, fp) == EOF)
		goto out;
	if (fclose(fp))
		goto out;
	fp = NULL;
	/* rename(2) atomically replaces an existing destination, so this is
	 * also how Add File / Update File's Move to overwrite in place -
	 * matching codex-rs, which does not guard against clobbering.
	 */
	if (rename(tmp, path))
		goto out;
	rc = 0;

out:
	if (fp)
		fclose(fp);
	if (fd >= 0)
		close(fd);
	if (rc)
		unlink(tmp);
	free(tmp);
	return rc;
}

static int append_range(struct response_buffer *buf, const char *p, size_t len)
{
	if (response_buffer_reserve(buf, buf->len + len + 1))
		return -1;
	memcpy(buf->data + buf->len, p, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	return 0;
}

static int buf_append_line(struct response_buffer *buf,
			   struct patch_line *line, size_t off)
{
	if (response_buffer_reserve(buf, buf->len + line->len - off + 2))
		return -1;
	memcpy(buf->data + buf->len, line->p + off, line->len - off);
	buf->len += line->len - off;
	buf->data[buf->len++] = '\n';
	buf->data[buf->len] = '\0';
	return 0;
}

/* ---- str_list ------------------------------------------------------- */

static int str_list_push(struct str_list *l, char *s)
{
	char **n;
	size_t newcap;

	if (!s)
		return -1;
	if (l->count == l->cap) {
		newcap = l->cap ? l->cap * 2 : 8;
		n = realloc(l->item, newcap * sizeof(*n));
		if (!n)
			return -1;
		l->item = n;
		l->cap = newcap;
	}
	l->item[l->count++] = s;
	return 0;
}

static void str_list_free(struct str_list *l)
{
	size_t i;

	for (i = 0; i < l->count; i++)
		free(l->item[i]);
	free(l->item);
	l->item = NULL;
	l->count = 0;
	l->cap = 0;
}

/*
 * Split file content into lines the way Rust's `str::split('\n')` does,
 * then drop exactly one trailing empty element - i.e. one trailing
 * newline is implicit and not represented as its own line. This matches
 * codex-rs's derive_new_contents_from_chunks() exactly, including its
 * quirk of collapsing a run of blank trailing lines by one.
 */
static int lines_from_content(const char *content, struct str_list *out)
{
	const char *p, *nl;
	size_t len;
	char *s;

	p = content;
	for (;;) {
		nl = strchr(p, '\n');
		len = nl ? (size_t)(nl - p) : strlen(p);
		s = malloc(len + 1);
		if (!s)
			return -1;
		memcpy(s, p, len);
		s[len] = '\0';
		if (str_list_push(out, s))
			return -1;
		if (!nl)
			break;
		p = nl + 1;
	}
	if (out->count && !out->item[out->count - 1][0]) {
		free(out->item[out->count - 1]);
		out->count--;
	}
	return 0;
}

static char *join_lines(struct str_list *lines)
{
	struct response_buffer out = {};
	size_t i;
	char *pad;

	if (!lines->count || lines->item[lines->count - 1][0] != '\0') {
		pad = strdup("");
		if (!pad || str_list_push(lines, pad))
			return NULL;
	}
	for (i = 0; i < lines->count; i++) {
		if (i && append_range(&out, "\n", 1))
			return NULL;
		if (append_range(&out, lines->item[i], strlen(lines->item[i])))
			return NULL;
	}
	return out.data ? out.data : strdup("");
}

static int str_list_splice(struct str_list *l, size_t idx, size_t old_len,
			    char **newitems, size_t new_count)
{
	char **n;
	size_t i, tail, need;

	for (i = 0; i < old_len; i++)
		free(l->item[idx + i]);
	tail = l->count - (idx + old_len);
	memmove(&l->item[idx], &l->item[idx + old_len], tail * sizeof(char *));
	l->count -= old_len;

	need = l->count + new_count;
	if (need > l->cap) {
		n = realloc(l->item, need * sizeof(*n));
		if (!n)
			return -1;
		l->item = n;
		l->cap = need;
	}
	memmove(&l->item[idx + new_count], &l->item[idx],
		(l->count - idx) * sizeof(char *));
	for (i = 0; i < new_count; i++) {
		l->item[idx + i] = strdup(newitems[i]);
		if (!l->item[idx + i])
			return -1;
	}
	l->count += new_count;
	return 0;
}

/* ---- context-anchored line matching (codex-rs seek_sequence) -------- */

/*
 * Find `pattern` (an ordered run of `pat_count` lines) within `lines`,
 * starting the search at line `start`. Tries, in order of decreasing
 * strictness: an exact match, trailing-whitespace-insensitive, fully
 * trimmed, then Unicode-punctuation-normalized. When `eof` is set the
 * search is pinned to the last `pat_count` lines of the file instead of
 * scanning forward from `start` - this is what lets a "*** End of File"
 * hunk anchor to the tail of the file. Returns the 0-based line index of
 * the match, or -1 if none of the four tiers found one.
 */
static long seek_sequence(struct str_list *lines, char **pattern,
			   size_t pat_count, size_t start, bool eof)
{
	size_t hi, search_start, i, j;
	char *a, *b;
	bool ok;

	if (pat_count == 0)
		return (long)start;
	if (pat_count > lines->count)
		return -1;
	hi = lines->count - pat_count;
	search_start = eof ? hi : start;

	for (i = search_start; i <= hi; i++) {
		for (j = 0; j < pat_count; j++)
			if (strcmp(lines->item[i + j], pattern[j]))
				break;
		if (j == pat_count)
			return (long)i;
	}
	for (i = search_start; i <= hi; i++) {
		ok = true;
		for (j = 0; j < pat_count; j++) {
			a = rstrip_dup(lines->item[i + j]);
			b = rstrip_dup(pattern[j]);
			if (strcmp(a, b))
				ok = false;
			free(a);
			free(b);
			if (!ok)
				break;
		}
		if (ok)
			return (long)i;
	}
	for (i = search_start; i <= hi; i++) {
		ok = true;
		for (j = 0; j < pat_count; j++) {
			a = trim_dup(lines->item[i + j]);
			b = trim_dup(pattern[j]);
			if (strcmp(a, b))
				ok = false;
			free(a);
			free(b);
			if (!ok)
				break;
		}
		if (ok)
			return (long)i;
	}
	for (i = search_start; i <= hi; i++) {
		ok = true;
		for (j = 0; j < pat_count; j++) {
			a = normalize_dup(lines->item[i + j]);
			b = normalize_dup(pattern[j]);
			if (!a || !b || strcmp(a, b))
				ok = false;
			free(a);
			free(b);
			if (!ok)
				break;
		}
		if (ok)
			return (long)i;
	}
	return -1;
}

/* ---- update chunk list ------------------------------------------------ */

static int chunk_list_push(struct update_chunk_list *l, char *context)
{
	struct update_chunk *n;
	size_t newcap;

	if (l->count == l->cap) {
		newcap = l->cap ? l->cap * 2 : 4;
		n = realloc(l->item, newcap * sizeof(*n));
		if (!n)
			return -1;
		l->item = n;
		l->cap = newcap;
	}
	memset(&l->item[l->count], 0, sizeof(l->item[l->count]));
	l->item[l->count].context = context;
	l->count++;
	return 0;
}

static void chunk_list_free(struct update_chunk_list *l)
{
	size_t i;

	for (i = 0; i < l->count; i++) {
		free(l->item[i].context);
		str_list_free(&l->item[i].old);
		str_list_free(&l->item[i].new);
	}
	free(l->item);
	l->item = NULL;
	l->count = 0;
	l->cap = 0;
}

static int repl_push(struct replacement **repl, size_t *count, size_t *cap,
		     size_t idx, size_t old_len, char **new_item,
		     size_t new_count)
{
	struct replacement *n;
	size_t newcap;

	if (*count == *cap) {
		newcap = *cap ? *cap * 2 : 4;
		n = realloc(*repl, newcap * sizeof(*n));
		if (!n)
			return -1;
		*repl = n;
		*cap = newcap;
	}
	(*repl)[*count].idx = idx;
	(*repl)[*count].old_len = old_len;
	(*repl)[*count].new_item = new_item;
	(*repl)[*count].new_count = new_count;
	(*count)++;
	return 0;
}

static int repl_cmp(const void *a, const void *b)
{
	const struct replacement *ra = a, *rb = b;

	if (ra->idx < rb->idx)
		return -1;
	if (ra->idx > rb->idx)
		return 1;
	return 0;
}

/*
 * Turn a hunk's chunk list into an ordered set of line-range splices
 * against `lines`, exactly as codex-rs's compute_replacements() does:
 * each chunk's change_context (if any) narrows the search window for the
 * chunks that follow it, a chunk with no old_lines is a pure insertion
 * appended at end-of-file, and a chunk with old_lines is matched via
 * seek_sequence and scheduled for replacement.
 */
static char *compute_replacements(struct str_list *lines,
				  struct update_chunk_list *chunks,
				  struct replacement **out_repl,
				  size_t *out_count)
{
	struct replacement *repl;
	size_t repl_count, repl_cap;
	size_t line_index;
	size_t i, insertion_idx, pat_count, new_count;
	struct update_chunk *ck;
	long idx;
	char *err;

	repl = NULL;
	repl_count = 0;
	repl_cap = 0;
	line_index = 0;

	for (i = 0; i < chunks->count; i++) {
		ck = &chunks->item[i];

		if (ck->context) {
			idx = seek_sequence(lines, &ck->context, 1,
					     line_index, false);
			if (idx < 0) {
				err = patch_errf("tool error: context '%s' not found",
						  ck->context);
				free(repl);
				return err ? err : patch_err("out of memory");
			}
			line_index = (size_t)idx + 1;
		}

		if (ck->old.count == 0) {
			insertion_idx = (lines->count &&
					  !lines->item[lines->count - 1][0]) ?
					 lines->count - 1 : lines->count;
			if (repl_push(&repl, &repl_count, &repl_cap,
				      insertion_idx, 0, ck->new.item,
				      ck->new.count)) {
				free(repl);
				return patch_err("out of memory");
			}
			continue;
		}

		pat_count = ck->old.count;
		new_count = ck->new.count;
		idx = seek_sequence(lines, ck->old.item, pat_count,
				     line_index, ck->is_eof);
		if (idx < 0 && pat_count && !ck->old.item[pat_count - 1][0]) {
			pat_count--;
			if (new_count && !ck->new.item[new_count - 1][0])
				new_count--;
			idx = seek_sequence(lines, ck->old.item, pat_count,
					     line_index, ck->is_eof);
		}
		if (idx < 0) {
			free(repl);
			return patch_err("hunk context not found");
		}
		if (repl_push(&repl, &repl_count, &repl_cap, (size_t)idx,
			      pat_count, ck->new.item, new_count)) {
			free(repl);
			return patch_err("out of memory");
		}
		line_index = (size_t)idx + pat_count;
	}

	qsort(repl, repl_count, sizeof(*repl), repl_cmp);
	*out_repl = repl;
	*out_count = repl_count;
	return NULL;
}

static char *apply_replacements(struct str_list *lines,
				struct replacement *repl, size_t count)
{
	size_t i;

	for (i = count; i-- > 0;) {
		if (str_list_splice(lines, repl[i].idx, repl[i].old_len,
				     repl[i].new_item, repl[i].new_count))
			return patch_err("out of memory");
	}
	return NULL;
}

/* ---- parsing ---------------------------------------------------------- */

static char *parse_update(struct patch_reader *r, const char *path,
			  struct patch_line *line, bool *have_line,
			  char **out_path, char **out_content)
{
	struct str_list lines = {};
	struct update_chunk_list chunks = {};
	struct replacement *repl = NULL;
	size_t repl_count = 0;
	struct update_chunk *ck;
	char *content;
	char *ltrim;
	char *ctx;
	char *text;
	char *err;
	const char *rest;
	bool saw_hunk;
	bool saw_marker;
	char c;

	content = read_text_file(path);
	if (!content)
		return patch_errno("read", path);
	if (lines_from_content(content, &lines)) {
		free(content);
		str_list_free(&lines);
		return patch_err("out of memory");
	}
	free(content);

	saw_hunk = false;
	saw_marker = false;
	for (;;) {
		if (*have_line)
			*have_line = false;
		else if (!patch_next_line(r, line))
			break;
		*have_line = true;

		ltrim = patch_line_trim_dup(line);
		if (!ltrim)
			goto oom;

		if (!saw_hunk && str_starts(ltrim, "*** Move to: ")) {
			free(*out_path);
			*out_path = strdup(ltrim + strlen("*** Move to: "));
			free(ltrim);
			if (!*out_path || !patch_path_ok(*out_path))
				goto invalid_path;
			*have_line = false;
			continue;
		}
		if (!strcmp(ltrim, "*** End of File")) {
			if (chunks.count)
				chunks.item[chunks.count - 1].is_eof = true;
			free(ltrim);
			*have_line = false;
			continue;
		}
		if (str_starts(ltrim, "*** ")) {
			saw_marker = true;
			free(ltrim);
			break;
		}
		if (str_starts(ltrim, "@@")) {
			rest = ltrim + 2;
			while (*rest == ' ')
				rest++;
			ctx = *rest ? strdup(rest) : NULL;
			if (*rest && !ctx) {
				free(ltrim);
				goto oom;
			}
			if (chunk_list_push(&chunks, ctx)) {
				free(ctx);
				free(ltrim);
				goto oom;
			}
			free(ltrim);
			saw_hunk = true;
			*have_line = false;
			continue;
		}
		free(ltrim);

		if (line->len < 1)
			goto malformed;
		c = line->p[0];
		if (c != ' ' && c != '-' && c != '+')
			goto malformed;
		if (!chunks.count && chunk_list_push(&chunks, NULL))
			goto oom;
		ck = &chunks.item[chunks.count - 1];
		if (c == ' ' || c == '-') {
			text = line_dup_offset(line, 1);
			if (!text || str_list_push(&ck->old, text))
				goto oom;
		}
		if (c == ' ' || c == '+') {
			text = line_dup_offset(line, 1);
			if (!text || str_list_push(&ck->new, text))
				goto oom;
		}
		saw_hunk = true;
		*have_line = false;
	}

	if (!saw_marker && !*have_line)
		goto malformed;
	if (!saw_hunk)
		goto empty_hunk;

	err = compute_replacements(&lines, &chunks, &repl, &repl_count);
	if (err)
		goto fail_err;
	err = apply_replacements(&lines, repl, repl_count);
	free(repl);
	if (err)
		goto fail_err;

	*out_content = join_lines(&lines);
	if (!*out_content) {
		err = patch_err("out of memory");
		goto fail_err;
	}

	str_list_free(&lines);
	chunk_list_free(&chunks);
	return NULL;

malformed:
	str_list_free(&lines);
	chunk_list_free(&chunks);
	return patch_err("malformed update hunk");
empty_hunk:
	str_list_free(&lines);
	chunk_list_free(&chunks);
	return patch_err("empty update hunk");
oom:
	str_list_free(&lines);
	chunk_list_free(&chunks);
	return patch_err("out of memory");
invalid_path:
	str_list_free(&lines);
	chunk_list_free(&chunks);
	free(*out_path);
	*out_path = NULL;
	return patch_err("invalid patch path");
fail_err:
	str_list_free(&lines);
	chunk_list_free(&chunks);
	return err;
}

static char *parse_add(struct patch_reader *r, const char *path,
		       struct patch_line *line, bool *have_line,
		       char **out_content)
{
	struct response_buffer content = {};
	bool saw_marker;

	(void)path;
	/* codex-rs's Add File silently overwrites an existing file (the
	 * mkstemp+rename in write_text_file_atomic() already does this at
	 * commit time); no existence check here.
	 */
	saw_marker = false;
	while (patch_next_line(r, line)) {
		*have_line = true;
		if (line_starts(line, "*** ")) {
			saw_marker = true;
			break;
		}
		if (line->len < 1 || line->p[0] != '+')
			goto malformed;
		if (buf_append_line(&content, line, 1))
			goto oom;
		*have_line = false;
	}

	if (!saw_marker)
		goto malformed;
	*out_content = content.data ? content.data : strdup("");
	if (!*out_content)
		goto oom;
	return NULL;

malformed:
	free(content.data);
	return patch_err("malformed add file");
oom:
	free(content.data);
	return patch_err("out of memory");
}

static char *parse_delete(const char *path)
{
	if (access(path, F_OK))
		return patch_errno("delete", path);
	return NULL;
}

static void patch_ops_free(struct patch_op *ops)
{
	struct patch_op *next;

	while (ops) {
		next = ops->next;
		free(ops->path);
		free(ops->new_path);
		free(ops->content);
		free(ops);
		ops = next;
	}
}

static char *patch_ops_append(struct patch_op **ops, struct patch_op **tail,
			      enum patch_op_kind kind, char *path,
			      char *new_path, char *content)
{
	struct patch_op *op;

	op = calloc(1, sizeof(*op));
	if (!op)
		return patch_err("out of memory");
	op->kind = kind;
	op->path = path;
	op->new_path = new_path;
	op->content = content;
	if (*tail)
		(*tail)->next = op;
	else
		*ops = op;
	*tail = op;
	return NULL;
}

static char *patch_ops_commit(struct patch_op *ops)
{
	struct patch_op *op;

	for (op = ops; op; op = op->next) {
		switch (op->kind) {
		case PATCH_OP_ADD:
			if (patch_ensure_parent_dir(op->path) ||
			    write_text_file_atomic(op->path, op->content))
				return patch_errno("write", op->path);
			break;
		case PATCH_OP_UPDATE:
			if (op->new_path) {
				if (patch_ensure_parent_dir(op->new_path) ||
				    write_text_file_atomic(op->new_path,
							   op->content))
					return patch_errno("write", op->new_path);
				if (strcmp(op->path, op->new_path) &&
				    unlink(op->path))
					return patch_errno("delete", op->path);
			} else if (write_text_file_atomic(op->path, op->content)) {
				return patch_errno("write", op->path);
			}
			break;
		case PATCH_OP_DELETE:
			if (unlink(op->path))
				return patch_errno("delete", op->path);
			break;
		}
	}
	return NULL;
}

char *fyai_apply_patch_text(const char *patch)
{
	struct patch_reader r = { .p = patch };
	struct patch_line line;
	struct patch_op *ops = NULL;
	struct patch_op *tail = NULL;
	char *path;
	char *new_path;
	char *content;
	char *err;
	char *ltrim;
	size_t changed;
	bool have_line;

	/* codex-rs tolerates leading/trailing whitespace around every "*** "
	 * marker line, so every marker comparison below runs against a
	 * trimmed copy rather than the raw line.
	 */
	if (!patch_next_line(&r, &line))
		return patch_err("patch must start with *** Begin Patch");
	ltrim = patch_line_trim_dup(&line);
	if (!ltrim || strcmp(ltrim, "*** Begin Patch")) {
		free(ltrim);
		return patch_err("patch must start with *** Begin Patch");
	}
	free(ltrim);

	changed = 0;
	have_line = false;
	for (;;) {
		if (!have_line && !patch_next_line(&r, &line))
			return patch_err("patch missing *** End Patch");
		have_line = false;

		ltrim = patch_line_trim_dup(&line);
		if (!ltrim) {
			patch_ops_free(ops);
			return patch_err("out of memory");
		}
		if (!strcmp(ltrim, "*** End Patch")) {
			free(ltrim);
			break;
		}
		new_path = NULL;
		content = NULL;
		if (str_starts(ltrim, "*** Add File: ")) {
			path = strdup(ltrim + strlen("*** Add File: "));
			free(ltrim);
			if (!path || !patch_path_ok(path)) {
				free(path);
				return patch_err("invalid patch path");
			}
			err = parse_add(&r, path, &line, &have_line,
					&content);
			if (!err)
				err = patch_ops_append(&ops, &tail, PATCH_OP_ADD,
						       path, NULL, content);
		} else if (str_starts(ltrim, "*** Delete File: ")) {
			path = strdup(ltrim + strlen("*** Delete File: "));
			free(ltrim);
			if (!path || !patch_path_ok(path)) {
				free(path);
				return patch_err("invalid patch path");
			}
			err = parse_delete(path);
			if (!err)
				err = patch_ops_append(&ops, &tail,
						       PATCH_OP_DELETE,
						       path, NULL, NULL);
		} else if (str_starts(ltrim, "*** Update File: ")) {
			path = strdup(ltrim + strlen("*** Update File: "));
			free(ltrim);
			if (!path || !patch_path_ok(path)) {
				free(path);
				return patch_err("invalid patch path");
			}
			err = parse_update(&r, path, &line, &have_line,
					   &new_path,
					   &content);
			if (!err)
				err = patch_ops_append(&ops, &tail,
						       PATCH_OP_UPDATE,
						       path, new_path,
						       content);
		} else {
			free(ltrim);
			patch_ops_free(ops);
			return patch_err("expected file operation");
		}
		if (err) {
			free(path);
			free(new_path);
			free(content);
			patch_ops_free(ops);
			return err;
		}
		changed++;
	}

	err = patch_ops_commit(ops);
	if (err) {
		patch_ops_free(ops);
		return err;
	}
	patch_ops_free(ops);
	if (asprintf(&err, "ok: %zu file%s changed", changed,
		     changed == 1 ? "" : "s") < 0)
		return NULL;
	return err;
}
