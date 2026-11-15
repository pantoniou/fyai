/* SPDX-License-Identifier: MIT */
#ifndef FYAI_IGNORE_H
#define FYAI_IGNORE_H

#include <stdbool.h>
#include <stddef.h>

#include <libfyaml/libfyaml-generic.h>

struct fyai_scratch;

/*
 * Ignore rules in the syntax of gitignore(5): blank lines and lines that start
 * with # are skipped, ! negates, a trailing / selects directories, a / at the
 * start or in the middle anchors the pattern to the directory of its file, and
 * * ? [] and ** have the meaning that git gives them. Trailing blanks that no
 * backslash protects are dropped.
 *
 * A path is tested as git tests it: the last rule that matches decides, and a
 * file nearer to the path decides before a file farther from it. A directory
 * that is ignored hides what it holds, so a caller does not descend into it
 * and does not test a path below it.
 */

/* The rules of one file, or of the configuration. */
struct fyai_ignore_scope {
	/* The file of the directory above, or NULL. */
	const struct fyai_ignore_scope *parent;
	/* The directory of the file, relative to the root, with no trailing slash; "" for the root. */
	const char *base;
	size_t base_length;
	struct fyai_ignore_rule *rules;
	size_t count;
};

/* The longest line that is a rule. A longer one is skipped. */
#define FYAI_IGNORE_LINE_MAX 4096

/*
 * Parse the length bytes of text into a scope in the arena. The scope, its rules
 * and their text live until the arena closes. Return NULL with errno set when
 * memory is short.
 */
struct fyai_ignore_scope *fyai_ignore_parse(struct fyai_scratch *scratch,
					    const struct fyai_ignore_scope *parent,
					    const char *base, const char *text, size_t length);

/* The same for rules that come as separate strings, as the configuration gives them. */
struct fyai_ignore_scope *fyai_ignore_parse_lines(struct fyai_scratch *scratch,
						  const char *const *lines, size_t count);

/*
 * Whether path, relative to the root and made of names that a slash separates,
 * is ignored. first is tested before scope and its parents: it holds the rules
 * that outrank every file, and can be NULL. Neither argument is changed.
 */
bool fyai_ignore_match(const struct fyai_ignore_scope *first, const struct fyai_ignore_scope *scope,
		       const char *path, bool is_dir);

/*
 * Read a file of ignore rules below the directory fd. A name that is absent, is not a
 * regular file or is a symbolic link has no rules: 0 with *text NULL. A file of more
 * than a mebibyte is an error (EFBIG). The caller frees the text.
 */
int fyai_ignore_read(int directory, const char *name, char **text, size_t *length);

/*
 * The rules that govern a directory tree on disk, tested for one path at a time, as
 * `git status` tests the paths of a work tree. The .gitignore file of each directory
 * on the way to a path is read when the path asks for it, from the root descriptor
 * and with no symbolic link followed, and kept. The root also reads
 * .git/info/exclude. A path is ignored when it or a directory above it is, because
 * an ignored directory hides what it holds.
 */
struct fyai_ignore_tree;

struct fyai_ignore_spec;

/*
 * Return the matcher, or NULL when the spec ignores nothing (no .gitignore files and
 * no rules) or memory is short (errno set). The descriptor is duplicated. The spec's
 * rules must outlive the matcher.
 */
struct fyai_ignore_tree *fyai_ignore_tree_open(const struct fyai_ignore_spec *spec, int root);
bool fyai_ignore_tree_match(struct fyai_ignore_tree *tree, const char *path, bool is_dir);
void fyai_ignore_tree_close(struct fyai_ignore_tree *tree);

/*
 * What a view ignores, as a capture takes it: whether the .gitignore files of the
 * project count, and the rules of the configuration. The rules are borrowed from
 * the record that loaded them, which must outlive the capture.
 */
struct fyai_ignore_spec {
	bool gitignore;
	/* A sequence of strings. A zeroed spec holds an empty one: no rules. */
	fy_generic patterns;
};

/*
 * The record that a view stores, built from the `view` section of the
 * configuration document: {gitignore, patterns}. The view keeps it, so the capture
 * of its result ignores what the capture of its baseline did.
 */
fy_generic fyai_ignore_record(struct fy_generic_builder *gb, fy_generic config);

/*
 * Load a record into a spec. A record that is not a mapping, such as the one of a
 * view that predates the setting, ignores nothing.
 */
void fyai_ignore_spec_load(struct fyai_ignore_spec *spec, fy_generic record);

#endif
