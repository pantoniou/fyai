/*
 * fyai_ignore.c - ignore rules in the syntax of gitignore(5)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_ignore.h"
#include "fyai_scratch.h"

struct fyai_ignore_rule {
	/* The pattern, split at its slashes. Each segment is a NUL-terminated string. */
	const char **segments;
	size_t segment_count;
	bool negate;
	bool dir_only;
	/* The pattern holds a slash: it matches the path from the directory of its file. */
	bool anchored;
};

/* Whether the pattern segment is the whole of "**". */
static bool ignore_double_star(const char *segment)
{
	return segment[0] == '*' && segment[1] == '*' && !segment[2];
}

/*
 * Match the segments of a pattern against the names of a path: a "**" segment
 * takes zero or more names, except at the end, where it takes at least one.
 */
static bool ignore_segments(const char *const *pattern, size_t pattern_count,
			    const char *const *names, size_t name_count)
{
	size_t i;

	if (!pattern_count)
		return !name_count;
	if (ignore_double_star(pattern[0])) {
		if (pattern_count == 1)
			return name_count >= 1;
		for (i = 0; i <= name_count; i++)
			if (ignore_segments(pattern + 1, pattern_count - 1, names + i,
					    name_count - i))
				return true;
		return false;
	}
	if (!name_count || fnmatch(pattern[0], names[0], 0))
		return false;
	return ignore_segments(pattern + 1, pattern_count - 1, names + 1, name_count - 1);
}

#define IGNORE_NAMES_MAX 256

/* Whether a rule matches the path below the directory of its file; rel is that path. */
static bool ignore_rule_match(const struct fyai_ignore_rule *rule, const char *rel, bool is_dir)
{
	const char *names[IGNORE_NAMES_MAX];
	char copy[PATH_MAX], *p;
	size_t count = 0;

	if (rule->dir_only && !is_dir)
		return false;
	/* A pattern with no slash names a file or a directory at any depth. */
	if (!rule->anchored) {
		p = strrchr(rel, '/');
		return !fnmatch(rule->segments[0], p ? p + 1 : rel, 0);
	}
	if (strlen(rel) >= sizeof(copy))
		return false;
	strcpy(copy, rel);
	for (p = copy;; p++) {
		if (count == IGNORE_NAMES_MAX)
			return false;
		names[count++] = p;
		p = strchr(p, '/');
		if (!p)
			break;
		*p = '\0';
	}
	return ignore_segments((const char *const *)rule->segments, rule->segment_count, names,
			       count);
}

/* The path below the directory of a scope, or NULL when the path is not below it. */
static const char *ignore_relative(const struct fyai_ignore_scope *scope, const char *path)
{
	if (!scope->base_length)
		return path;
	if (strncmp(path, scope->base, scope->base_length) || path[scope->base_length] != '/')
		return NULL;
	return path + scope->base_length + 1;
}

/* The decision of one scope: 1 ignored, 0 un-ignored by a negation, -1 no rule matched. */
static int ignore_scope_decide(const struct fyai_ignore_scope *scope, const char *path, bool is_dir)
{
	const char *rel;
	size_t i;

	rel = ignore_relative(scope, path);
	if (!rel || !*rel)
		return -1;
	/* The last rule that matches decides. */
	for (i = scope->count; i--;)
		if (ignore_rule_match(&scope->rules[i], rel, is_dir))
			return scope->rules[i].negate ? 0 : 1;
	return -1;
}

bool fyai_ignore_match(const struct fyai_ignore_scope *first, const struct fyai_ignore_scope *scope,
		       const char *path, bool is_dir)
{
	int decision;

	if (first) {
		decision = ignore_scope_decide(first, path, is_dir);
		if (decision >= 0)
			return decision;
	}
	for (; scope; scope = scope->parent) {
		decision = ignore_scope_decide(scope, path, is_dir);
		if (decision >= 0)
			return decision;
	}
	return false;
}

/* Parse one line into a rule in the arena. Return 1 for a rule, 0 for none, -1 for no memory. */
static int ignore_rule_parse(struct fyai_scratch *scratch, const char *line, size_t length,
			     struct fyai_ignore_rule *rule)
{
	char *text, *p, *end, *start;
	size_t count;

	/* A trailing carriage return belongs to the line end. */
	if (length && line[length - 1] == '\r')
		length--;
	/* Trailing blanks are dropped, unless a backslash protects the last one. */
	while (length && line[length - 1] == ' ' && !(length > 1 && line[length - 2] == '\\'))
		length--;
	if (!length || line[0] == '#' || length > FYAI_IGNORE_LINE_MAX)
		return 0;
	text = fyai_scratch_strndup(scratch, line, length);
	if (!text)
		return -1;
	memset(rule, 0, sizeof(*rule));
	p = text;
	if (*p == '!') {
		rule->negate = true;
		p++;
	}
	end = p + strlen(p);
	if (end > p && end[-1] == '/') {
		rule->dir_only = true;
		*--end = '\0';
	}
	if (*p == '/') {
		rule->anchored = true;
		p++;
	}
	if (!*p)
		return 0;
	if (strchr(p, '/'))
		rule->anchored = true;
	count = 1;
	for (start = p; *start; start++)
		if (*start == '/')
			count++;
	rule->segments = fyai_scratch_alloc(scratch, count * sizeof(*rule->segments));
	if (!rule->segments)
		return -1;
	/* An empty segment, as in a//b, is the same as one slash: it names nothing. */
	for (count = 0; p;) {
		start = p;
		p = strchr(p, '/');
		if (p)
			*p++ = '\0';
		if (*start)
			rule->segments[count++] = start;
	}
	if (!count)
		return 0;
	rule->segment_count = count;
	return 1;
}

struct fyai_ignore_scope *fyai_ignore_parse(struct fyai_scratch *scratch,
					    const struct fyai_ignore_scope *parent,
					    const char *base, const char *text, size_t length)
{
	struct fyai_ignore_scope *scope;
	struct fyai_ignore_rule rule, *rules = NULL;
	const char *line, *end, *stop = text + length;
	size_t capacity = 0, count = 0, line_count = 1;
	int rc;

	scope = fyai_scratch_calloc(scratch, 1, sizeof(*scope));
	if (!scope)
		return NULL;
	scope->parent = parent;
	scope->base = base ? fyai_scratch_strndup(scratch, base, strlen(base)) : "";
	if (!scope->base)
		return NULL;
	scope->base_length = strlen(scope->base);
	for (end = text; end < stop; end++)
		if (*end == '\n')
			line_count++;
	rules = fyai_scratch_calloc(scratch, line_count, sizeof(*rules));
	if (!rules)
		return NULL;
	capacity = line_count;
	for (line = text; line < stop; line = end + 1) {
		end = memchr(line, '\n', (size_t)(stop - line));
		if (!end)
			end = stop;
		rc = ignore_rule_parse(scratch, line, (size_t)(end - line), &rule);
		if (rc < 0)
			return NULL;
		if (rc && count < capacity)
			rules[count++] = rule;
	}
	scope->rules = rules;
	scope->count = count;
	return scope;
}

struct fyai_ignore_scope *fyai_ignore_parse_lines(struct fyai_scratch *scratch,
						  const char *const *lines, size_t count)
{
	struct fyai_ignore_scope *scope;
	struct fyai_ignore_rule rule;
	size_t i, used = 0;
	int rc;

	scope = fyai_scratch_calloc(scratch, 1, sizeof(*scope));
	if (!scope)
		return NULL;
	scope->base = "";
	scope->rules = fyai_scratch_calloc(scratch, count ? count : 1, sizeof(*scope->rules));
	if (!scope->rules)
		return NULL;
	for (i = 0; i < count; i++) {
		rc = ignore_rule_parse(scratch, lines[i], strlen(lines[i]), &rule);
		if (rc < 0)
			return NULL;
		if (rc)
			scope->rules[used++] = rule;
	}
	scope->count = used;
	return scope;
}

fy_generic fyai_ignore_record(struct fy_generic_builder *gb, fy_generic config)
{
	fy_generic section = fy_get(config, "view", fy_invalid);
	fy_generic patterns = fy_get(section, "ignore", fy_seq_empty);

	return fy_mapping(gb, "gitignore", (bool)fy_get(section, "gitignore", true),
			  "patterns", fy_is_sequence(patterns) ? patterns : fy_seq_empty);
}

void fyai_ignore_spec_load(struct fyai_ignore_spec *spec, fy_generic record)
{
	fy_generic patterns;

	spec->gitignore = false;
	spec->patterns = fy_seq_empty;
	if (!fy_is_mapping(record))
		return;
	spec->gitignore = fy_get(record, "gitignore", false);
	patterns = fy_get(record, "patterns", fy_seq_empty);
	if (fy_is_sequence(patterns))
		spec->patterns = patterns;
}

/* The ignore files are small; a larger one is not a list of rules. */
#define IGNORE_FILE_MAX (1024 * 1024)

int fyai_ignore_read(int directory, const char *name, char **text, size_t *length)
{
	struct stat st;
	ssize_t n = 0;
	size_t used = 0;
	char *buffer = NULL;
	int fd, saved, rc;

	*text = NULL;
	*length = 0;
	fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return errno == ENOENT || errno == ENOTDIR || errno == ELOOP || errno == ENXIO ? 0 : -1;
	rc = fstat(fd, &st);
	if (rc)
		goto err_out;
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return 0;
	}
	if (st.st_size > IGNORE_FILE_MAX) {
		errno = EFBIG;
		goto err_out;
	}
	buffer = malloc((size_t)st.st_size + 1);
	if (!buffer)
		goto err_out;
	while (used < (size_t)st.st_size) {
		n = read(fd, buffer + used, (size_t)st.st_size - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (size_t)n;
	}
	if (n < 0)
		goto err_out;
	close(fd);
	*text = buffer;
	*length = used;
	return 0;

err_out:
	saved = errno;
	free(buffer);
	close(fd);
	errno = saved;
	return -1;
}

struct ignore_dir {
	char *path;
	const struct fyai_ignore_scope *scope;
};

struct fyai_ignore_tree {
	struct fyai_scratch scratch;
	const struct fyai_ignore_scope *config;
	bool gitignore;
	int root;
	struct ignore_dir *dirs;
	size_t count, capacity;
};

/* Open a directory below the root by its relative path, with no symbolic link followed. */
static int ignore_tree_open_dir(const struct fyai_ignore_tree *tree, const char *path)
{
	char copy[PATH_MAX], *name, *next;
	int fd, child;

	if (strlen(path) >= sizeof(copy)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(copy, path);
	fd = fcntl(tree->root, F_DUPFD_CLOEXEC, 3);
	for (name = copy; fd >= 0 && *name; name = next) {
		next = strchr(name, '/');
		if (next)
			*next++ = '\0';
		else
			next = name + strlen(name);
		child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(fd);
		fd = child;
	}
	return fd;
}

/* The scope in force for the children of a directory; the directories above are loaded first. */
static const struct fyai_ignore_scope *ignore_tree_scope(struct fyai_ignore_tree *tree, const char *dir)
{
	static const char *const names[] = { ".git/info/exclude", ".gitignore" };
	const struct fyai_ignore_scope *parent = NULL, *scope;
	struct ignore_dir *grown;
	char up[PATH_MAX], *slash, *text, *copy;
	size_t i, length;
	int fd;

	for (i = 0; i < tree->count; i++)
		if (!strcmp(tree->dirs[i].path, dir))
			return tree->dirs[i].scope;
	if (*dir) {
		snprintf(up, sizeof(up), "%s", dir);
		slash = strrchr(up, '/');
		if (slash)
			*slash = '\0';
		else
			*up = '\0';
		parent = ignore_tree_scope(tree, up);
	}
	scope = parent;
	fd = ignore_tree_open_dir(tree, dir);
	for (i = *dir ? 1 : 0; fd >= 0 && i < 2; i++) {
		if (fyai_ignore_read(fd, names[i], &text, &length) || !text)
			continue;
		scope = fyai_ignore_parse(&tree->scratch, scope, dir, text, length);
		free(text);
		if (!scope)
			break;
	}
	if (fd >= 0)
		close(fd);
	if (!scope)
		scope = parent;
	if (tree->count == tree->capacity) {
		grown = fyai_scratch_grow(&tree->scratch, tree->dirs, tree->count,
					  tree->capacity ? tree->capacity * 2 : 16, sizeof(*grown));
		if (!grown)
			return scope;
		tree->dirs = grown;
		tree->capacity = tree->capacity ? tree->capacity * 2 : 16;
	}
	length = strlen(dir);
	copy = fyai_scratch_strndup(&tree->scratch, dir, length);
	if (!copy)
		return scope;
	tree->dirs[tree->count].path = copy;
	tree->dirs[tree->count++].scope = scope;
	return scope;
}

struct fyai_ignore_tree *fyai_ignore_tree_open(const struct fyai_ignore_spec *spec, int root)
{
	struct fyai_ignore_tree *tree;
	const char **lines;
	const char *pattern;
	size_t count, used = 0;

	if (!spec || (!spec->gitignore && !fy_len(spec->patterns)))
		return NULL;
	tree = calloc(1, sizeof(*tree));
	if (!tree)
		return NULL;
	tree->root = -1;
	if (fyai_scratch_open(&tree->scratch)) {
		free(tree);
		return NULL;
	}
	tree->gitignore = spec->gitignore;
	count = fy_len(spec->patterns);
	if (count) {
		lines = fyai_scratch_calloc(&tree->scratch, count, sizeof(*lines));
		if (!lines)
			goto fail;
		fy_foreach(pattern, spec->patterns)
			lines[used++] = pattern;
		tree->config = fyai_ignore_parse_lines(&tree->scratch, lines, used);
		if (!tree->config)
			goto fail;
	}
	tree->root = fcntl(root, F_DUPFD_CLOEXEC, 3);
	if (tree->root < 0)
		goto fail;
	return tree;
fail:
	fyai_ignore_tree_close(tree);
	return NULL;
}

bool fyai_ignore_tree_match(struct fyai_ignore_tree *tree, const char *path, bool is_dir)
{
	char prefix[PATH_MAX], *cut;
	const struct fyai_ignore_scope *scope;
	const char *slash;
	size_t length;

	if (!tree || !*path || strlen(path) >= sizeof(prefix))
		return false;
	/* From the top: a directory that is ignored hides everything below it. */
	for (slash = path;; slash++) {
		slash = strchr(slash, '/');
		length = slash ? (size_t)(slash - path) : strlen(path);
		memcpy(prefix, path, length);
		prefix[length] = '\0';
		scope = NULL;
		if (tree->gitignore) {
			/* The scope of the directory that holds this name. */
			cut = strrchr(prefix, '/');
			if (cut) {
				*cut = '\0';
				scope = ignore_tree_scope(tree, prefix);
				*cut = '/';
			} else {
				scope = ignore_tree_scope(tree, "");
			}
		}
		if (fyai_ignore_match(tree->config, scope, prefix, slash ? true : is_dir))
			return true;
		if (!slash)
			return false;
	}
}

void fyai_ignore_tree_close(struct fyai_ignore_tree *tree)
{
	if (!tree)
		return;
	if (tree->root >= 0)
		close(tree->root);
	fyai_scratch_close(&tree->scratch);
	free(tree);
}
