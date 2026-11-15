/*
 * fyai_project_diff.c - compare two recorded project roots
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_project.h"

struct project_diff_item {
	char *hex;
	char *path;
	fy_generic value;
};

struct project_diff_tree {
	struct project_diff_item *items;
	size_t count, capacity;
};

static int diff_name(const char *hex, char **name)
{
	size_t i, length = strlen(hex);
	unsigned int byte;
	const char *high, *low, *digits = "0123456789abcdef";
	char *text;

	if (!length || (length & 1)) {
		errno = EINVAL;
		return -1;
	}
	text = malloc(length / 2 + 1);
	if (!text)
		return -1;
	for (i = 0; i < length; i += 2) {
		high = strchr(digits, hex[i]);
		low = strchr(digits, hex[i + 1]);
		byte = high && low ? ((high - digits) << 4) | (low - digits) : 0;
		if (!high || !low || !byte || byte == '/') {
			free(text);
			errno = EINVAL;
			return -1;
		}
		text[i / 2] = byte;
	}
	text[length / 2] = '\0';
	if (!strcmp(text, ".") || !strcmp(text, "..")) {
		free(text);
		errno = EINVAL;
		return -1;
	}
	*name = text;
	return 0;
}

static fy_generic diff_value(struct fy_generic_builder *gb, fy_generic snapshot, fy_generic object,
			     const char *hex)
{
	fy_generic value, attrs, blob;
	const char *kind = fy_get(object, "kind", "");

	attrs = fyai_project_attributes(snapshot, hex, object);
	value = fy_mapping(gb, "kind", fy_value(gb, kind), "mode", fy_get(object, "mode", 0LL),
			   "uid", fy_get(object, "uid", 0LL), "gid", fy_get(object, "gid", 0LL),
			   "mtime_sec", fy_get(attrs, "mtime_sec", 0LL), "mtime_nsec",
			   fy_get(attrs, "mtime_nsec", 0LL));
	if (!strcmp(kind, "file")) {
		blob = fy_get(object, "blob", fy_invalid);
		value = fy_assoc(gb, value, "digest", fy_get(blob, "digest", ""));
		value = fy_assoc(gb, value, "size", fy_get(blob, "size", 0LL));
	} else if (!strcmp(kind, "symlink"))
		value = fy_assoc(gb, value, "target_hex", fy_get(object, "target_hex", ""));
	else if (strcmp(kind, "directory")) {
		errno = EINVAL;
		return fy_invalid;
	}
	return value;
}

static int diff_collect(struct fy_generic_builder *gb, struct project_diff_tree *tree,
			fy_generic snapshot, fy_generic object, const char *hex, const char *path,
			unsigned int depth)
{
	struct project_diff_item *items, *item;
	fy_generic entry, child;
	char *name = NULL, *child_hex = NULL, *child_path = NULL;
	const char *encoded;
	int rc = -1, saved;

	if (depth > FYAI_PROJECT_MAX_DEPTH || tree->count >= FYAI_PROJECT_MAX_NODES ||
	    !fy_is_mapping(object)) {
		errno = EINVAL;
		return -1;
	}
	if (tree->count == tree->capacity) {
		tree->capacity = tree->capacity ? tree->capacity * 2 : 64;
		items = realloc(tree->items, tree->capacity * sizeof(*items));
		if (!items)
			return -1;
		tree->items = items;
	}
	item = &tree->items[tree->count++];
	item->hex = strdup(hex);
	item->path = strdup(path);
	item->value = diff_value(gb, snapshot, object, hex);
	if (!item->hex || !item->path || !fy_is_valid(item->value))
		return -1;
	if (!fy_equal(fy_get(object, "kind", ""), "directory"))
		return 0;
	fy_foreach(entry, fy_get(object, "entries", fy_invalid)) {
		encoded = fy_get(entry, "name_hex", "");
		if (!strcmp(encoded, "2e676974") || !strcmp(encoded, "2e66796169"))
			continue;
		if (diff_name(encoded, &name))
			goto out;
		if (asprintf(&child_hex, "%s%s%s", hex, *hex ? FYAI_PROJECT_HEX_SLASH : "", encoded) < 0 ||
		    asprintf(&child_path, "%s%s%s", path, *path ? "/" : "", name) < 0)
			goto out;
		child = fyai_project_get(fy_get(snapshot, "objects", fy_invalid),
					 fy_get(entry, "digest", ""));
		rc = diff_collect(gb, tree, snapshot, child, child_hex, child_path, depth + 1);
		free(name);
		free(child_hex);
		free(child_path);
		name = child_hex = child_path = NULL;
		if (rc)
			goto out;
	}
	rc = 0;
out:
	saved = errno;
	free(name);
	free(child_hex);
	free(child_path);
	errno = saved;
	return rc;
}

static int diff_order(const void *a, const void *b)
{
	const struct project_diff_item *left = a, *right = b;

	return strcmp(left->hex, right->hex);
}

fy_generic fyai_project_diff(struct fy_generic_builder *gb, fy_generic left, fy_generic right)
{
	struct project_diff_tree trees[2] = { 0 };
	fy_generic snapshots[2] = { left, right }, root, result = fy_invalid, *rows = NULL;
	struct project_diff_item *a, *b, *item;
	size_t i, j, k, count = 0;
	int order, saved;

	for (k = 0; k < 2; k++) {
		if (!fy_equal(fy_get(snapshots[k], "version", fy_invalid), 2LL)) {
			errno = EINVAL;
			goto out;
		}
		root = fyai_project_get(fy_get(snapshots[k], "objects", fy_invalid),
					fy_get(snapshots[k], "root", ""));
		if (diff_collect(gb, &trees[k], snapshots[k], root, "", "", 0))
			goto out;
		qsort(trees[k].items, trees[k].count, sizeof(*trees[k].items), diff_order);
	}
	rows = calloc(trees[0].count + trees[1].count, sizeof(*rows));
	if (!rows)
		goto out;
	for (i = j = 0; i < trees[0].count || j < trees[1].count;) {
		a = i < trees[0].count ? &trees[0].items[i] : NULL;
		b = j < trees[1].count ? &trees[1].items[j] : NULL;
		order = !a ? 1 : !b ? -1 : strcmp(a->hex, b->hex);
		item = order > 0 ? b : a;
		if (order || !fy_equal(a->value, b->value))
			rows[count++] =
				fy_mapping(gb, "path", fy_value(gb, *item->path ? item->path : "."),
					   "path_hex", fy_value(gb, item->hex), "status",
					   order < 0 ? "deleted" :
					   order > 0 ? "added" :
						       "modified",
					   "before", order > 0 ? fy_null : a->value, "after",
					   order < 0 ? fy_null : b->value);
		if (order <= 0)
			i++;
		if (order >= 0)
			j++;
		if (count && !fy_is_valid(rows[count - 1])) {
			errno = ENOMEM;
			goto out;
		}
	}
	result = fy_gb_sequence_create(gb, count, rows);
out:
	saved = errno;
	free(rows);
	for (k = 0; k < 2; k++) {
		for (i = 0; i < trees[k].count; i++) {
			free(trees[k].items[i].hex);
			free(trees[k].items[i].path);
		}
		free(trees[k].items);
	}
	errno = saved;
	return result;
}
