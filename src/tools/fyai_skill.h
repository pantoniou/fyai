/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SKILL_H
#define FYAI_SKILL_H

#include <stdbool.h>
#include <stddef.h>

#include "fyai.h"

/*
 * Skills are branch state, as the configuration is: the store member `skills`
 * maps a name to {frontmatter, body, files, source, imported}. `frontmatter`
 * is the front matter of SKILL.md as written, `body` the Markdown after it,
 * and `files` maps a relative path to the text of a supporting file. A skill
 * enters the store only through an import: nothing is discovered from the
 * filesystem. See doc/skills.md.
 */

/* Longest skill name, as the Agent Skills format allows. */
#define FYAI_SKILL_NAME_MAX	64
/* Largest SKILL.md and largest supporting file kept. */
#define FYAI_SKILL_TEXT_MAX	(256 * 1024)
/* Most supporting files of one skill. */
#define FYAI_SKILL_FILES_MAX	256
/* Largest sum of the texts of one skill. */
#define FYAI_SKILL_TOTAL_MAX	(8 * 1024 * 1024)

/* The strings of @seq joined with @sep, built in @gb. */
fy_generic fyai_skill_join(struct fy_generic_builder *gb, fy_generic seq,
			   const char *sep);

/* Remove the directory that this invocation wrote its skills out to. */
void fyai_skill_dirs_release(struct fyai_ctx *ctx);

/* Forget that directory without removing it: a forked child does not own it. */
void fyai_skill_dirs_abandon(struct fyai_ctx *ctx);

/* Whether @name is a usable skill name: [a-z0-9][a-z0-9-]*, 64 at most. */
bool fyai_skill_name_valid(const char *name);

/* The skills of the active branch, a mapping, or fy_invalid. */
fy_generic fyai_skills(struct fyai_ctx *ctx);

/* The stored skill @name, or fy_invalid. Borrowed from the store. */
fy_generic fyai_skill_get(struct fyai_ctx *ctx, const char *name);

/*
 * Split @text, the content of a SKILL.md, into its front matter and its body,
 * and check the front matter against data/skill.schema.yaml. @dirname names
 * the skill when the front matter does not. Returns {frontmatter, body} built
 * in @gb, with `name` set in the front matter, or fy_invalid with the cause in
 * @why.
 */
fy_generic fyai_skill_parse(struct fy_generic_builder *gb, const char *text,
			    const char *dirname, char *why, size_t why_size);

/*
 * Read the skill or skills at @path: a SKILL.md file, a directory that holds
 * one, or a directory of such directories. Store them on the branch in one
 * publish. A stored skill of the same name is replaced only with @force.
 * Sets *@namesp to the sequence of the imported names, built in @gb.
 * Returns 0, or -1 with a diagnostic.
 */
int fyai_skill_import(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		      const char *path, bool force, fy_generic *namesp);

/*
 * Replace the SKILL.md of skill @name with @text, or, with @file, its
 * supporting file @file, and publish. @exec sets (1) or clears (0) the
 * executable bit of @file, or keeps it (-1). With @create the skill must not exist,
 * and it starts with no files. @text is checked as an import checks it: a
 * front matter that does not match the schema, or that names another skill,
 * is refused. Returns 0, or -1 with a diagnostic.
 */
int fyai_skill_update(struct fyai_ctx *ctx, const char *name,
		      const char *file, const char *text, bool create,
		      int exec);

/* The SKILL.md that a new skill @name starts from, interned in @gb. */
const char *fyai_skill_template(struct fy_generic_builder *gb,
				const char *name);

/*
 * Store the text file @path as the supporting file @rel of skill @name, or
 * under its base name when @rel is NULL, and publish. 0, or -1.
 */
int fyai_skill_import_file(struct fyai_ctx *ctx, const char *name,
			   const char *path, const char *rel);

/* Remove the supporting file @rel of skill @name and publish. 0, or -1. */
int fyai_skill_remove_file(struct fyai_ctx *ctx, const char *name,
			   const char *rel);

/*
 * Import the skills of Claude Code in @scope: "project" (.claude in the
 * current directory), "user" ($CLAUDE_CONFIG_DIR or ~/.claude, with the
 * skills that claude.ai synced), "plugins" (the installed plugins, each name
 * after the name of its plugin), or "all", in that order. A skills directory
 * gives a skill per SKILL.md, and a commands directory a skill per Markdown
 * file. The first source of a name wins. A skill that cannot be read is
 * skipped with a warning. Publish once and set *@namesp as
 * fyai_skill_import() does. Returns 0, or -1 with a diagnostic.
 */
int fyai_skill_import_claude(struct fyai_ctx *ctx,
			     struct fy_generic_builder *gb, const char *scope,
			     bool force, fy_generic *namesp);

/*
 * Enable or disable the skills named in the sequence @names and publish. A
 * disabled skill stays stored, and neither the model nor /NAME runs it.
 * Returns 0, or -1 with a diagnostic.
 */
int fyai_skill_set_enabled(struct fyai_ctx *ctx, fy_generic names,
			   bool enabled);

/* Remove the skills named in the sequence @names and publish. 0, or -1. */
int fyai_skill_remove(struct fyai_ctx *ctx, fy_generic names);

/*
 * Write skill @name as @dir/SKILL.md and its supporting files under @dir.
 * The directory is created; an existing file is not replaced. 0, or -1.
 */
int fyai_skill_export(struct fyai_ctx *ctx, const char *name, const char *dir);

/*
 * Write the supporting file @rel of skill @name as @dir/@rel. The directories
 * are created; an existing file is not replaced. 0, or -1.
 */
int fyai_skill_export_file(struct fyai_ctx *ctx, const char *name,
			   const char *rel, const char *dir);

/*
 * The supporting files of skill @name as rows {path, executable, bytes,
 * lines}, built in @gb.
 */
fy_generic fyai_skill_file_rows(struct fyai_ctx *ctx,
				struct fy_generic_builder *gb, const char *name);

/* The stored skills as rows for `skill list`, built in @gb. */
fy_generic fyai_skill_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb);

/* The text of SKILL.md of skill @name, rebuilt in @gb, or fy_invalid. */
fy_generic fyai_skill_source(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			     fy_generic skill);

/*
 * Substitute @args in the body of @skill as Claude Code does: $ARGUMENTS,
 * $ARGUMENTS[N], $N, and ${CLAUDE_SKILL_DIR}. A body that names no argument
 * gets the arguments in a closing line. Built in @gb.
 */
fy_generic fyai_skill_expand(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			     fy_generic skill, const char *args);

/*
 * The model message for the line @line that the user typed, when it is
 * "/NAME ARGS" and NAME is a skill that the user can invoke and no command
 * of the session. Built in @gb. fy_invalid when the line is not a skill.
 */
fy_generic fyai_skill_user_message(struct fyai_ctx *ctx,
				   struct fy_generic_builder *gb,
				   const char *line);

/* Whether @line is "/skill exec NAME ARGS", which runs a skill whatever commands there are. */
bool fyai_skill_exec_line(const char *line);

/* Whether @line is "/NAME ARGS" and runs a skill that fyai_skill_user_message() expands. */
bool fyai_skill_user_line(struct fyai_ctx *ctx, const char *line);

/*
 * The task of a sub-agent that runs skill @name with @args: the loaded skill,
 * with its files written out in this process. Built in @gb, or fy_invalid
 * with a diagnostic.
 */
fy_generic fyai_skill_task(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			   const char *name, const char *args);

/* Whether @skill runs as a sub-agent (`context: fork`). */
bool fyai_skill_forks(fy_generic skill);

/*
 * Map the `allowed-tools` of @frontmatter to canonical fyai tool names, a
 * sequence built in @gb, or fy_invalid when the skill names none. A name
 * that maps to no fyai tool is left out and named in @dropped, which may be
 * NULL.
 */
fy_generic fyai_skill_allowed_tools(struct fy_generic_builder *gb,
				    fy_generic frontmatter,
				    fy_generic *dropped);

/* Whether the tool allow list of this run admits @name. */
bool fyai_skill_tool_allowed(struct fyai_ctx *ctx, const char *name);

/* The names of the skills the user can invoke, for completion, in @gb. */
fy_generic fyai_skill_user_names(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb);

#endif
