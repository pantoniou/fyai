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

static bool line_eq(struct patch_line *line, const char *s)
{
	size_t len;

	len = strlen(s);
	return line->len == len && !memcmp(line->p, s, len);
}

static bool line_starts(struct patch_line *line, const char *s)
{
	size_t len;

	len = strlen(s);
	return line->len >= len && !memcmp(line->p, s, len);
}

static char *line_suffix_dup(struct patch_line *line, const char *prefix)
{
	size_t prefix_len;
	size_t len;
	char *out;

	prefix_len = strlen(prefix);
	if (line->len < prefix_len)
		return NULL;
	len = line->len - prefix_len;
	out = malloc(len + 1);
	if (!out)
		return NULL;
	memcpy(out, line->p + prefix_len, len);
	out[len] = '\0';
	return out;
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

static char *patch_err(const char *msg)
{
	char *out;

	if (asprintf(&out, "tool error: %s", msg) < 0)
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

static char *apply_hunk(char *content, struct response_buffer *old,
			struct response_buffer *new, char **cursor)
{
	struct response_buffer out = {};
	char *match;
	size_t prefix_len;

	match = strstr(*cursor, old->data ? old->data : "");
	if (!match)
		return NULL;

	prefix_len = (size_t)(match - content);
	if (append_range(&out, content, prefix_len) ||
	    append_range(&out, new->data ? new->data : "", new->len) ||
	    append_range(&out, match + old->len, strlen(match + old->len))) {
		free(out.data);
		return NULL;
	}

	*cursor = out.data + prefix_len + new->len;
	free(content);
	return out.data;
}

static char *parse_update(struct patch_reader *r, const char *path,
			  struct patch_line *line, bool *have_line,
			  char **out_path, char **out_content)
{
	struct response_buffer old = {};
	struct response_buffer new = {};
	char *content;
	char *cursor;
	char *next;
	bool saw_hunk;
	bool saw_marker;

	content = read_text_file(path);
	if (!content)
		return patch_errno("read", path);

	cursor = content;
	saw_hunk = false;
	saw_marker = false;
	for (;;) {
		if (*have_line)
			*have_line = false;
		else if (!patch_next_line(r, line))
			break;
		*have_line = true;
		if (!saw_hunk && line_starts(line, "*** Move to: ")) {
			*out_path = line_suffix_dup(line, "*** Move to: ");
			if (!patch_path_ok(*out_path))
				goto invalid_path;
			if (strcmp(path, *out_path) && !access(*out_path, F_OK))
				goto exists;
			*have_line = false;
			continue;
		}
		if (line_starts(line, "*** ")) {
			saw_marker = true;
			break;
		}
		if (!line_starts(line, "@@"))
			goto malformed;

		free(old.data);
		free(new.data);
		old = (struct response_buffer){};
		new = (struct response_buffer){};
		while (patch_next_line(r, line)) {
			*have_line = true;
			if (line_eq(line, "*** End of File")) {
				*have_line = false;
				break;
			}
			if (line_starts(line, "@@") || line_starts(line, "*** "))
				break;
			if (line->len < 1)
				goto malformed;
			switch (line->p[0]) {
			case ' ':
				if (buf_append_line(&old, line, 1) ||
				    buf_append_line(&new, line, 1))
					goto oom;
				break;
			case '-':
				if (buf_append_line(&old, line, 1))
					goto oom;
				break;
			case '+':
				if (buf_append_line(&new, line, 1))
					goto oom;
				break;
			default:
				goto malformed;
			}
			*have_line = false;
		}
		next = apply_hunk(content, &old, &new, &cursor);
		if (!next) {
			free(content);
			free(old.data);
			free(new.data);
			return patch_err("hunk context not found");
		}
		content = next;
		saw_hunk = true;
		if (*have_line && line_starts(line, "*** "))
			break;
	}

	if (!saw_marker && !*have_line)
		goto malformed;
	*out_content = content;
	free(old.data);
	free(new.data);
	return NULL;

malformed:
	free(content);
	free(old.data);
	free(new.data);
	return patch_err("malformed update hunk");
oom:
	free(content);
	free(old.data);
	free(new.data);
	return patch_err("out of memory");
invalid_path:
	free(content);
	free(old.data);
	free(new.data);
	free(*out_path);
	*out_path = NULL;
	return patch_err("invalid patch path");
exists:
	free(content);
	free(old.data);
	free(new.data);
	free(*out_path);
	*out_path = NULL;
	return patch_err("move target already exists");
}

static char *parse_add(struct patch_reader *r, const char *path,
		       struct patch_line *line, bool *have_line,
		       char **out_content)
{
	struct response_buffer content = {};
	FILE *fp;
	bool saw_marker;

	fp = fopen(path, "rb");
	if (fp) {
		fclose(fp);
		return patch_err("add file already exists");
	}

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
	size_t changed;
	bool have_line;

	if (!patch_next_line(&r, &line) || !line_eq(&line, "*** Begin Patch"))
		return patch_err("patch must start with *** Begin Patch");

	changed = 0;
	have_line = false;
	for (;;) {
		if (!have_line && !patch_next_line(&r, &line))
			return patch_err("patch missing *** End Patch");
		have_line = false;

		if (line_eq(&line, "*** End Patch"))
			break;
		new_path = NULL;
		content = NULL;
		if (line_starts(&line, "*** Add File: ")) {
			path = line_suffix_dup(&line, "*** Add File: ");
			if (!patch_path_ok(path)) {
				free(path);
				return patch_err("invalid patch path");
			}
			err = parse_add(&r, path, &line, &have_line,
					&content);
			if (!err)
				err = patch_ops_append(&ops, &tail, PATCH_OP_ADD,
						       path, NULL, content);
		} else if (line_starts(&line, "*** Delete File: ")) {
			path = line_suffix_dup(&line, "*** Delete File: ");
			if (!patch_path_ok(path)) {
				free(path);
				return patch_err("invalid patch path");
			}
			err = parse_delete(path);
			if (!err)
				err = patch_ops_append(&ops, &tail,
						       PATCH_OP_DELETE,
						       path, NULL, NULL);
		} else if (line_starts(&line, "*** Update File: ")) {
			path = line_suffix_dup(&line, "*** Update File: ");
			if (!patch_path_ok(path)) {
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
