/*
 * fyai_skill.c - branch-stored skills and the skill tool
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_SKILL

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dirent.h>
#include <ftw.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_cmd.h"
#include "fyai_config.h"
#include "fyai_diag.h"
#include "fyai_schema.h"
#include "fyai_skill.h"
#include "fyai_storage.h"
#include "fyai_tool_registry.h"
#include "fyai_tools.h"
#include "utils.h"

/* FYAI_EMBEDDED_SKILL_SCHEMA[] - data/skill.schema.yaml. */
#include "embedded_skill_schema.inc"

/* Deepest directory of supporting files that an import descends. */
#define SKILL_DIR_DEPTH_MAX	8

/* Longest agent name that a forked skill takes: the branch component limit. */
#define SKILL_AGENT_NAME_MAX	48

bool fyai_skill_name_valid(const char *name)
{
	size_t i;

	if (fy_str_empty(name) || strlen(name) > FYAI_SKILL_NAME_MAX)
		return false;
	for (i = 0; name[i]; i++) {
		if ((name[i] >= 'a' && name[i] <= 'z') ||
		    (name[i] >= '0' && name[i] <= '9'))
			continue;
		if (name[i] == '-' && i)
			continue;
		return false;
	}
	return true;
}

fy_generic fyai_skills(struct fyai_ctx *ctx)
{
	if (!ctx || !fy_is_mapping(ctx->arena_skills))
		return fy_invalid;
	return ctx->arena_skills;
}

fy_generic fyai_skill_get(struct fyai_ctx *ctx, const char *name)
{
	fy_generic skills, skill;

	skills = fyai_skills(ctx);
	if (fy_is_invalid(skills) || fy_str_empty(name))
		return fy_invalid;
	skill = fy_get(skills, name, fy_invalid);
	return fy_is_mapping(skill) ? skill : fy_invalid;
}

static fy_generic skill_frontmatter(fy_generic skill)
{
	fy_generic fm = fy_get(skill, "frontmatter", fy_invalid);

	return fy_is_mapping(fm) ? fm : fy_map_empty;
}

/*
 * The mode that a file of @skill is created with: executable when the import
 * found it executable, as the store keeps it in `executables`. The umask
 * applies.
 */
static mode_t skill_file_mode(fy_generic skill, const char *rel)
{
	fy_generic item;

	fy_foreach(item, fy_get(skill, "executables", fy_seq_empty))
		if (fy_equal(item, rel))
			return 0777;
	return 0666;
}

/* Set or clear @rel in the executables of @skill. */
static fy_generic skill_set_exec(struct fy_generic_builder *gb,
				 fy_generic skill, const char *rel, bool exec)
{
	fy_generic execs, out, item;

	execs = fy_get(skill, "executables", fy_seq_empty);
	out = fy_seq_empty;
	fy_foreach(item, fy_is_sequence(execs) ? execs : fy_seq_empty)
		if (!fy_equal(item, rel))
			out = fy_append(gb, out, item);
	if (exec)
		out = fy_append(gb, out, fy_value(gb, rel));
	if (fy_is_invalid(out))
		return fy_invalid;
	return fy_empty(out) ? fy_disassoc(gb, skill, "executables") :
		fy_assoc(gb, skill, "executables", out);
}

bool fyai_skill_forks(fy_generic skill)
{
	return fy_equal(fy_get(skill_frontmatter(skill), "context", fy_invalid),
			"fork");
}

/*
 * Whether the user enabled @skill. The state is a member of the stored entry,
 * not of the front matter, which stays as its author wrote it.
 */
static bool skill_enabled(fy_generic skill)
{
	return fy_get(skill, "enabled", true);
}

/* Whether the front matter lets the model load @skill. */
static bool skill_fm_model(fy_generic skill)
{
	return !fy_get(skill_frontmatter(skill), "disable-model-invocation",
		       false);
}

/* Whether the front matter lets the user run @skill as /NAME. */
static bool skill_fm_user(fy_generic skill)
{
	return fy_get(skill_frontmatter(skill), "user-invocable", true);
}

static bool skill_model_invocable(fy_generic skill)
{
	return skill_enabled(skill) && skill_fm_model(skill);
}

static bool skill_user_invocable(fy_generic skill)
{
	return skill_enabled(skill) && skill_fm_user(skill);
}

fy_generic fyai_skill_join(struct fy_generic_builder *gb, fy_generic seq,
			   const char *sep)
{
	struct fyai_buf out = {0};
	fy_generic item, v;
	bool first;
	int rc;

	rc = 0;
	first = true;
	fy_foreach(item, fy_is_sequence(seq) ? seq : fy_seq_empty) {
		rc = (!first && fyai_buf_append(&out, sep)) ||
		     fyai_buf_append(&out, fy_castp(&item, ""));
		if (rc)
			break;
		first = false;
	}
	v = rc ? fy_invalid : fy_value(gb, out.data ? out.data : "");
	v = fy_gb_internalize(gb, v);
	free(out.data);
	return v;
}

/* ---- parsing and checking ------------------------------------------------ */

/*
 * The embedded schema, parsed once for the process. It lives in a builder of
 * its own: the builder of the first caller can be released before the next
 * call.
 */
static fy_generic skill_schema(void)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	static struct fy_generic_builder *schema_gb;
	static fy_generic schema = fy_invalid;
	fy_generic_sized_string embedded;

	if (fy_is_valid(schema))
		return schema;
	if (!schema_gb)
		schema_gb = fy_generic_builder_create(&cfg);
	if (!schema_gb)
		return fy_invalid;
	embedded.data = (const char *)FYAI_EMBEDDED_SKILL_SCHEMA;
	embedded.size = FYAI_EMBEDDED_SKILL_SCHEMA_LEN;
	schema = fy_parse(schema_gb, embedded,
			  FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING,
			  NULL);
	return schema;
}

/* Find the line "---" that closes the front matter at @p. */
static const char *skill_fence_end(const char *p)
{
	while (*p) {
		if (!strncmp(p, "---", 3) &&
		    (p[3] == '\n' || p[3] == '\0' ||
		     (p[3] == '\r' && p[4] == '\n')))
			return p;
		p = strchr(p, '\n');
		if (!p)
			return NULL;
		p++;
	}
	return NULL;
}

fy_generic fyai_skill_parse(struct fy_generic_builder *gb, const char *text,
			    const char *dirname, char *why, size_t why_size)
{
	fy_generic_sized_string yaml;
	fy_generic fm, schema, report, name_v;
	const char *start, *end, *body;
	const char *name;
	char *problems;

	if (!text) {
		snprintf(why, why_size, "no text");
		return fy_invalid;
	}
	/* A byte order mark is not part of the fence. */
	if (!strncmp(text, "\xef\xbb\xbf", 3))
		text += 3;
	if (strncmp(text, "---", 3) ||
	    (text[3] != '\n' && !(text[3] == '\r' && text[4] == '\n'))) {
		snprintf(why, why_size, "SKILL.md must start with a front "
			 "matter between two lines of '---'");
		return fy_invalid;
	}
	start = strchr(text, '\n') + 1;
	end = skill_fence_end(start);
	if (!end) {
		snprintf(why, why_size, "the front matter has no closing '---'");
		return fy_invalid;
	}
	body = end + 3;
	if (*body == '\r')
		body++;
	if (*body == '\n')
		body++;

	yaml.data = start;
	yaml.size = (size_t)(end - start);
	fm = yaml.size ? fy_parse(gb, yaml,
				  FYAI_YAML_PARSE_FLAGS |
				  FYOPPF_INPUT_TYPE_STRING, NULL) :
			 fy_map_empty;
	if (!fy_is_mapping(fm)) {
		snprintf(why, why_size, "the front matter is not a YAML "
			 "mapping");
		return fy_invalid;
	}
	schema = skill_schema();
	if (fy_is_invalid(schema)) {
		snprintf(why, why_size, "the skill schema cannot be read");
		return fy_invalid;
	}
	report = fyai_schema_validate(gb, schema, fm);
	if (!fyai_schema_valid(report)) {
		problems = fyai_schema_report_string(report);
		snprintf(why, why_size, "the front matter does not match the "
			 "skill schema: %s", problems ? problems : "unknown");
		free(problems);
		return fy_invalid;
	}

	name_v = fy_get(fm, "name", fy_invalid);
	name = fy_is_string(name_v) ? fy_castp(&name_v, "") : dirname;
	if (!fyai_skill_name_valid(name)) {
		snprintf(why, why_size, "'%s' is not a usable skill name; use "
			 "lowercase letters, digits and '-', at most %d",
			 name ? name : "", FYAI_SKILL_NAME_MAX);
		return fy_invalid;
	}
	if (!fy_is_string(name_v))
		fm = fy_assoc(gb, fm, "name", fy_value(gb, name));
	if (fy_is_invalid(fm)) {
		snprintf(why, why_size, "out of memory");
		return fy_invalid;
	}
	return fy_mapping(gb, "frontmatter", fm, "body", fy_value(gb, body));
}

/* ---- import -------------------------------------------------------------- */

static bool skill_rel_path_ok(const char *rel);

/* The state of one import: the skills read so far and their names. */
struct skill_import {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *gb;
	fy_generic skills;	/* the store mapping being built */
	fy_generic names;	/* the names imported, in order */
	const char *prefix;	/* put before each name, or NULL */
	size_t kept;		/* the skills found and left as stored */
	/* A SKILL.md named on its own: its directory names it, nothing more. */
	bool no_files;
	bool force;
	/* A skill that cannot be read is skipped with a warning, not fatal. */
	bool lenient;
};

/*
 * Report why a skill of the import cannot be read: a warning when the import
 * skips it and goes on, else the error that stops the import.
 */
#define skill_fail(_im, _fmt, ...) \
	fyai_diag_type(fyai_ctx_diag((_im)->ctx), \
		       (_im)->lenient ? FYAIET_WARNING : FYAIET_ERROR, \
		       (_fmt) , ## __VA_ARGS__)

/*
 * Add the supporting files under @dir (relative path @rel) to *@filesp, and
 * the path of each one its owner can execute to *@execsp.
 */
static int skill_read_files(struct skill_import *im, const char *dir,
			    const char *rel, int depth, fy_generic *filesp,
			    fy_generic *execsp, size_t *totalp)
{
	struct fyai_ctx *ctx = im->ctx;
	struct dirent *de;
	struct stat st;
	char path[PATH_MAX], sub[PATH_MAX];
	char *text;
	size_t full;
	DIR *d;
	int rc;

	d = opendir(dir);
	if (!d) {
		skill_fail(im, "cannot read the directory %s: %s", dir,
			   strerror(errno));
		goto err;
	}
	rc = 0;
	while (!rc && (de = readdir(d))) {
		/* Hidden entries, such as .git, are not part of a skill. */
		if (de->d_name[0] == '.')
			continue;
		if (!*rel && !strcmp(de->d_name, "SKILL.md"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		snprintf(sub, sizeof(sub), "%s%s%s", rel, *rel ? "/" : "",
			 de->d_name);
		if (lstat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth >= SKILL_DIR_DEPTH_MAX) {
				fyai_notice(ctx, "%s is too deep; "
					    "left out", path);
				continue;
			}
			rc = skill_read_files(im, path, sub, depth + 1,
					      filesp, execsp, totalp);
			continue;
		}
		/* A link is not followed: it can name a file outside. */
		if (!S_ISREG(st.st_mode))
			continue;
		if (fy_len(*filesp) >= FYAI_SKILL_FILES_MAX) {
			skill_fail(im, "%s holds more than %d supporting files",
				   dir, FYAI_SKILL_FILES_MAX);
			rc = -1;
			break;
		}
		text = read_text_file_limited(path, FYAI_SKILL_TEXT_MAX, &full);
		if (!text) {
			skill_fail(im, "cannot read %s: %s", path,
				   strerror(errno));
			rc = -1;
			break;
		}
		if (full > FYAI_SKILL_TEXT_MAX ||
		    data_is_binary(text, strlen(text)) ||
		    strlen(text) != full) {
			fyai_notice(ctx, "%s is not a text file of at "
				    "most %d bytes; left out", path,
				    FYAI_SKILL_TEXT_MAX);
			free(text);
			continue;
		}
		*totalp += full;
		if (*totalp > FYAI_SKILL_TOTAL_MAX) {
			free(text);
			skill_fail(im, "the files under %s exceed %d bytes",
				   dir, FYAI_SKILL_TOTAL_MAX);
			rc = -1;
			break;
		}
		*filesp = fy_assoc(im->gb, *filesp, fy_value(im->gb, sub),
				   fy_value(im->gb, text));
		free(text);
		if (st.st_mode & S_IXUSR)
			*execsp = fy_append(im->gb, *execsp,
					    fy_value(im->gb, sub));
		fyai_error_check(ctx, fy_is_valid(*filesp) &&
				 fy_is_valid(*execsp), err_close,
				 "cannot keep %s", path);
	}
	closedir(d);
	return rc;

err_close:
	closedir(d);
err:
	return -1;
}

/* Report what in @frontmatter has no effect in fyai. */
static void skill_import_notes(struct fyai_ctx *ctx, const char *name,
			       fy_generic fm)
{
	fy_generic dropped, item;
	bool forks;

	forks = fy_equal(fy_get(fm, "context", fy_invalid), "fork");
	if (!forks) {
		if (fy_is_valid(fy_get(fm, "model", fy_invalid)))
			fyai_notice(ctx, "'%s': model applies only with "
				    "context: fork", name);
		if (fy_is_valid(fy_get(fm, "allowed-tools", fy_invalid)))
			fyai_notice(ctx, "'%s': allowed-tools applies "
				    "only with context: fork", name);
		return;
	}
	dropped = fy_seq_empty;
	(void)fyai_skill_allowed_tools(fyai_ctx_transient_gb(ctx), fm,
				       &dropped);
	fy_foreach(item, dropped)
		fyai_notice(ctx, "'%s': allowed-tools '%s' names no fyai "
			    "tool; left out", name, fy_castp(&item, ""));
}

/* Read the skill whose SKILL.md is @md, in directory @dir (NULL: single file). */
static int skill_import_one(struct skill_import *im, const char *md,
			    const char *dir)
{
	struct fyai_ctx *ctx = im->ctx;
	struct fy_generic_builder *gb = im->gb;
	fy_generic parsed, fm, name_v, files, execs, entry, item;
	char why[1024], base[PATH_MAX], absdir[PATH_MAX];
	const char *slash, *dirname, *name;
	char *text, *dot;
	size_t full, total;
	int rc;

	text = read_text_file_limited(md, FYAI_SKILL_TEXT_MAX, &full);
	if (!text) {
		skill_fail(im, "cannot read %s: %s", md, strerror(errno));
		return -1;
	}
	if (full > FYAI_SKILL_TEXT_MAX || strlen(text) != full) {
		free(text);
		skill_fail(im, "%s is not a text file of at most %d bytes",
			   md, FYAI_SKILL_TEXT_MAX);
		return -1;
	}
	/* The directory names the skill; a single file names it by its stem. */
	snprintf(base, sizeof(base), "%s", dir ? dir : md);
	while (strlen(base) > 1 && base[strlen(base) - 1] == '/')
		base[strlen(base) - 1] = '\0';
	slash = strrchr(base, '/');
	dirname = slash ? slash + 1 : base;
	if (!dir && (dot = strrchr(base, '.')) && dot > dirname)
		*dot = '\0';
	parsed = fyai_skill_parse(gb, text, dirname, why, sizeof(why));
	free(text);
	if (fy_is_invalid(parsed)) {
		skill_fail(im, "%s: %s", md, why);
		return -1;
	}
	fm = fy_get(parsed, "frontmatter", fy_invalid);
	name_v = fy_get(fm, "name", fy_invalid);
	name = fy_castp(&name_v, "");
	/* A prefix keeps the skills of a plugin apart; the name says so. */
	if (im->prefix) {
		name = fy_gb_intern_string(gb, fy_sprintfa("%s-%s", im->prefix,
							   name));
		if (!fyai_skill_name_valid(name)) {
			skill_fail(im, "%s: '%s' is not a usable skill name",
				   md, name);
			return -1;
		}
		fm = fy_assoc(gb, fm, "name", fy_value(gb, name));
		fyai_error_check(ctx, fy_is_mapping(fm), err,
				 "cannot keep the skill '%s'", name);
	}

	/* The first source of a name in one import wins. */
	fy_foreach(item, im->names) {
		if (!fy_equal(item, name))
			continue;
		fyai_notice(ctx, "'%s' of %s is left out: an earlier source of "
			    "this import has the name", name, md);
		return 0;
	}
	if (!im->force &&
	    fy_is_valid(fy_get(im->skills, name, fy_invalid))) {
		if (!im->lenient) {
			fyai_error(ctx, "'%s' is already stored; use --force to "
				   "replace it", name);
			return -1;
		}
		fyai_notice(ctx, "kept the stored skill '%s' over %s; --force "
			    "replaces it", name, md);
		im->kept++;
		return 0;
	}
	if (fy_is_valid(fyai_cmd_session_lookup(name)))
		fyai_notice(ctx, "'%s': a command of the same name takes "
			    "/%s; the model can still load the skill", name,
			    name);
	skill_import_notes(ctx, name, fm);

	files = fy_map_empty;
	execs = fy_seq_empty;
	total = full;
	if (dir && !im->no_files) {
		rc = skill_read_files(im, dir, "", 0, &files, &execs, &total);
		if (rc)
			return -1;
	}
	if (!realpath(dir ? dir : md, absdir))
		snprintf(absdir, sizeof(absdir), "%s", dir ? dir : md);
	entry = fy_mapping(gb,
		"frontmatter", fm,
		"body", fy_get(parsed, "body", ""),
		"source", fy_value(gb, absdir),
		"imported", fy_value(gb, (long long)fyai_branch_timestamp()));
	if (fy_is_valid(entry) && !fy_empty(files))
		entry = fy_assoc(gb, entry, "files", files);
	if (fy_is_valid(entry) && !fy_empty(execs))
		entry = fy_assoc(gb, entry, "executables", execs);
	im->skills = fy_assoc(gb, im->skills, fy_value(gb, name), entry);
	im->names = fy_append(gb, im->names, fy_value(gb, name));
	fyai_error_check(ctx, fy_is_valid(im->skills) &&
			 fy_is_valid(im->names), err,
			 "cannot keep the skill '%s'", name);
	return 0;

err:
	return -1;
}

static bool skill_dir_has_md(const char *dir, char *md, size_t md_size)
{
	int n;

	n = snprintf(md, md_size, "%s/SKILL.md", dir);
	if (n < 0 || (size_t)n >= md_size)
		return false;
	return is_mode_file(md, R_OK);
}

/* Publish @skills, built in the transient builder, as the skills of the branch. */
static int skill_publish(struct fyai_ctx *ctx, fy_generic skills)
{
	fy_generic durable;

	fyai_error_check(ctx, ctx->durable_gb, err,
			 "no arena; run fyai init");
	durable = fy_empty(skills) ? fy_invalid :
		fy_gb_internalize(ctx->gb, skills);
	fyai_error_check(ctx, fy_empty(skills) || fy_is_valid(durable), err,
			 "cannot keep the skills in the arena");
	ctx->arena_skills = durable;
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_SKILL, NULL);
	if (fyai_publish_state(ctx)) {
		fyai_error(ctx, "could not publish the skills of branch "
			   "'%s'", fyai_ctx_branch(ctx));
		return -1;
	}
	/* The tool list holds the skill listing: offer the new set at once. */
	if (fyai_request_state_apply(ctx)) {
		fyai_error(ctx, "could not offer the skills of branch '%s' "
			   "to the model", fyai_ctx_branch(ctx));
		return -1;
	}
	return 0;

err:
	return -1;
}

int fyai_skill_import(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		      const char *path, bool force, fy_generic *namesp)
{
	struct skill_import im = {
		.ctx = ctx,
		.gb = gb,
		.force = force,
	};
	char md[PATH_MAX], sub[PATH_MAX];
	struct dirent *de;
	struct stat st;
	DIR *d;
	int rc;

	*namesp = fy_seq_empty;
	im.skills = fy_is_mapping(ctx->arena_skills) ?
		ctx->arena_skills : fy_map_empty;
	im.names = fy_seq_empty;
	rc = stat(path, &st);
	fyai_error_check(ctx, !rc, err, "cannot read %s: %s", path,
			 strerror(errno));
	if (S_ISREG(st.st_mode)) {
		/*
		 * A file is the skill alone: SKILL.md is named by its
		 * directory, another file by its stem. The supporting files
		 * come only with an import of the directory.
		 */
		/* The full path, so a SKILL.md in "." is named by its directory. */
		if (!realpath(path, sub))
			snprintf(sub, sizeof(sub), "%s", path);
		if (!strcmp(strrchr(sub, '/') ? strrchr(sub, '/') + 1 : sub,
			    "SKILL.md")) {
			if (strrchr(sub, '/'))
				*strrchr(sub, '/') = '\0';
			im.no_files = true;
			rc = skill_import_one(&im, path, sub);
		} else {
			rc = skill_import_one(&im, path, NULL);
		}
	} else if (skill_dir_has_md(path, md, sizeof(md))) {
		rc = skill_import_one(&im, md, path);
	} else {
		d = opendir(path);
		fyai_error_check(ctx, d, err, "cannot read %s: %s",
				 path, strerror(errno));
		rc = 0;
		while (!rc && (de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
			if (!skill_dir_has_md(sub, md, sizeof(md)))
				continue;
			rc = skill_import_one(&im, md, sub);
		}
		closedir(d);
		fyai_error_check(ctx, rc || !fy_empty(im.names), err,
				 "%s holds no SKILL.md, and no "
				 "directory under it holds one", path);
	}
	if (rc)
		return -1;
	rc = skill_publish(ctx, im.skills);
	if (rc)
		return -1;
	*namesp = im.names;
	return 0;

err:
	return -1;
}

/* ---- Claude Code skills -------------------------------------------------- */

/*
 * Import one skill of a bulk import. A skill that cannot be read is skipped
 * with the warning that says why; a failure that raised an error, such as a
 * resource that cannot be had, stops the import.
 */
static int skill_import_try(struct skill_import *im, const char *md,
			    const char *dir)
{
	int rc;

	rc = skill_import_one(im, md, dir);
	if (rc && !fyai_diag_got_error(&im->ctx->cfg->diag))
		return 0;
	return rc;
}

/* Import each directory under @dir that holds a SKILL.md. */
static int skill_scan_skills(struct skill_import *im, const char *dir)
{
	char md[PATH_MAX], sub[PATH_MAX];
	struct dirent *de;
	DIR *d;
	int rc;

	d = opendir(dir);
	if (!d)
		return 0;
	rc = 0;
	while (!rc && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(sub, sizeof(sub), "%s/%s", dir, de->d_name);
		if (skill_dir_has_md(sub, md, sizeof(md)))
			rc = skill_import_try(im, md, sub);
	}
	closedir(d);
	return rc;
}

/*
 * Import each Markdown file under the commands directory @dir as a skill
 * named by its stem: Claude Code runs a command file as a skill. A
 * subdirectory groups commands and does not change their names.
 */
static int skill_scan_commands(struct skill_import *im, const char *dir,
			       int depth)
{
	char path[PATH_MAX];
	struct dirent *de;
	struct stat st;
	size_t len;
	DIR *d;
	int rc;

	d = opendir(dir);
	if (!d)
		return 0;
	rc = 0;
	while (!rc && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (lstat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode) && depth < SKILL_DIR_DEPTH_MAX) {
			rc = skill_scan_commands(im, path, depth + 1);
			continue;
		}
		len = strlen(de->d_name);
		if (S_ISREG(st.st_mode) && len > 3 &&
		    !strcmp(de->d_name + len - 3, ".md"))
			rc = skill_import_try(im, path, NULL);
	}
	closedir(d);
	return rc;
}

/* The skills and the commands of the Claude Code directory @base. */
static int skill_scan_claude_dir(struct skill_import *im, const char *base)
{
	char path[PATH_MAX];
	int rc, n;

	/* A path longer than PATH_MAX names nothing that can be opened. */
	n = snprintf(path, sizeof(path), "%s/skills", base);
	if (n < 0 || (size_t)n >= sizeof(path))
		return 0;
	rc = skill_scan_skills(im, path);
	if (!rc) {
		n = snprintf(path, sizeof(path), "%s/commands", base);
		if (n < 0 || (size_t)n >= sizeof(path))
			return 0;
		rc = skill_scan_commands(im, path, 0);
	}
	return rc;
}

/* The skills that claude.ai synced: one directory of skills per bucket. */
static int skill_scan_synced(struct skill_import *im, const char *base)
{
	char path[PATH_MAX], bucket[PATH_MAX];
	struct dirent *de;
	DIR *d;
	int rc, n;

	n = snprintf(path, sizeof(path), "%s/skills/synced", base);
	if (n < 0 || (size_t)n >= sizeof(path))
		return 0;
	d = opendir(path);
	if (!d)
		return 0;
	rc = 0;
	while (!rc && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (snprintf(bucket, sizeof(bucket), "%s/%s", path,
			     de->d_name) >= (int)sizeof(bucket))
			continue;
		rc = skill_scan_skills(im, bucket);
	}
	closedir(d);
	return rc;
}

/*
 * The skills and commands of each installed plugin, as installed_plugins.json
 * lists them. The name of the plugin is put before each name, as Claude Code
 * puts it before the name of a plugin skill.
 */
static int skill_scan_plugins(struct skill_import *im, const char *base)
{
	struct fyai_ctx *ctx = im->ctx;
	fy_generic doc, plugins, key, installs, inst, ipath;
	char path[PATH_MAX], prefix[FYAI_SKILL_NAME_MAX + 1];
	const char *name, *at, *dir;
	int rc;

	snprintf(path, sizeof(path), "%s/plugins/installed_plugins.json", base);
	if (access(path, R_OK))
		return 0;
	doc = fy_parse_file(im->gb, FYAI_YAML_PARSE_FLAGS, path);
	if (!fy_is_mapping(doc)) {
		fyai_warning(ctx, "%s is not a JSON object; no plugin skill "
			     "is imported", path);
		return 0;
	}
	plugins = fy_get(doc, "plugins", fy_invalid);
	rc = 0;
	fy_foreach_key_value(key, installs, fy_is_mapping(plugins) ? plugins :
			     fy_map_empty) {
		name = fy_castp(&key, "");
		at = strchr(name, '@');
		snprintf(prefix, sizeof(prefix), "%.*s",
			 (int)(at ? at - name : (ptrdiff_t)strlen(name)), name);
		fy_foreach(inst, fy_is_sequence(installs) ? installs :
			   fy_seq_empty) {
			ipath = fy_get(inst, "installPath", fy_invalid);
			dir = fy_castp(&ipath, "");
			if (!*dir)
				continue;
			im->prefix = prefix;
			rc = skill_scan_claude_dir(im, dir);
			im->prefix = NULL;
			if (rc)
				return rc;
		}
	}
	return rc;
}

/* The directory of the Claude Code configuration: $CLAUDE_CONFIG_DIR, else ~/.claude. */
static const char *skill_claude_base(char *buf, size_t size)
{
	const char *env = getenv("CLAUDE_CONFIG_DIR");
	const char *home = getenv("HOME");

	if (!fy_str_empty(env))
		snprintf(buf, size, "%s", env);
	else if (!fy_str_empty(home))
		snprintf(buf, size, "%s/.claude", home);
	else
		return NULL;
	return buf;
}

int fyai_skill_import_claude(struct fyai_ctx *ctx,
			     struct fy_generic_builder *gb, const char *scope,
			     bool force, fy_generic *namesp)
{
	struct skill_import im = {
		.ctx = ctx,
		.gb = gb,
		.force = force,
		.lenient = true,
	};
	char base[PATH_MAX], cwd[PATH_MAX], project[PATH_MAX];
	const char *dir;
	bool all;
	int rc;

	*namesp = fy_seq_empty;
	im.skills = fy_is_mapping(ctx->arena_skills) ?
		ctx->arena_skills : fy_map_empty;
	im.names = fy_seq_empty;
	all = !strcmp(scope, "all");
	rc = 0;
	/* The project goes first: its skills win over the user's, as in Claude Code. */
	if (all || !strcmp(scope, "project")) {
		dir = getcwd(cwd, sizeof(cwd));
		fyai_error_check(ctx, dir, err,
				 "cannot read the current directory: %s",
				 strerror(errno));
		rc = snprintf(project, sizeof(project), "%s/.claude", cwd);
		fyai_error_check(ctx, rc < (int)sizeof(project), err,
				 "the current directory is too long: %s", cwd);
		rc = skill_scan_claude_dir(&im, project);
	}
	if (!rc && (all || !strcmp(scope, "user") ||
		    !strcmp(scope, "plugins"))) {
		dir = skill_claude_base(base, sizeof(base));
		fyai_error_check(ctx, dir,
				 err, "neither $CLAUDE_CONFIG_DIR nor $HOME "
				 "names the Claude Code directory");
		if (all || !strcmp(scope, "user")) {
			rc = skill_scan_claude_dir(&im, base);
			if (!rc)
				rc = skill_scan_synced(&im, base);
		}
		if (!rc && (all || !strcmp(scope, "plugins")))
			rc = skill_scan_plugins(&im, base);
	}
	if (rc)
		return -1;
	fyai_error_check(ctx, !fy_empty(im.names) || im.kept, err,
			 "found no Claude Code skill to import (%s)", scope);
	/* Every skill found is stored already: there is nothing to publish. */
	if (fy_empty(im.names))
		return 0;
	rc = skill_publish(ctx, im.skills);
	if (rc)
		return -1;
	*namesp = im.names;
	return 0;

err:
	return -1;
}

int fyai_skill_import_file(struct fyai_ctx *ctx, const char *name,
			   const char *path, const char *rel)
{
	const char *base;
	struct stat st;
	char *text;
	size_t full;
	bool exec;
	int rc;

	if (!rel) {
		base = strrchr(path, '/');
		rel = base ? base + 1 : path;
	}
	fyai_error_check(ctx, strcmp(rel, "SKILL.md"), err,
			 "SKILL.md is the skill itself; edit it with skill "
			 "edit %s, or import its directory with --force", name);
	text = read_text_file_limited(path, FYAI_SKILL_TEXT_MAX, &full);
	fyai_error_check(ctx, text, err, "cannot read %s: %s", path,
			 strerror(errno));
	if (full > FYAI_SKILL_TEXT_MAX || strlen(text) != full) {
		free(text);
		fyai_error(ctx, "%s is not a text file of at most %d bytes",
			   path, FYAI_SKILL_TEXT_MAX);
		return -1;
	}
	/* The file keeps the executable bit it has. */
	rc = stat(path, &st);
	exec = !rc && (st.st_mode & S_IXUSR);
	rc = fyai_skill_update(ctx, name, rel, text, false, exec);
	free(text);
	return rc;

err:
	return -1;
}

int fyai_skill_remove_file(struct fyai_ctx *ctx, const char *name,
			   const char *rel)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic skill, files, skills;

	skill = fyai_skill_get(ctx, name);
	fyai_error_check(ctx, fy_is_valid(skill), err,
			 "no skill '%s' on branch '%s'", name,
			 fyai_ctx_branch(ctx));
	files = fy_get(skill, "files", fy_invalid);
	fyai_error_check(ctx, fy_is_mapping(files) &&
			 fy_is_valid(fy_get(files, rel, fy_invalid)), err,
			 "skill '%s' has no file '%s'", name, rel);
	files = fy_disassoc(gb, files, fy_value(gb, rel));
	skill = fy_empty(files) ? fy_disassoc(gb, skill, "files") :
		fy_assoc(gb, skill, "files", files);
	skill = skill_set_exec(gb, skill, rel, false);
	skills = fy_assoc(gb, fyai_skills(ctx), fy_value(gb, name), skill);
	fyai_error_check(ctx, fy_is_mapping(skills), err,
			 "cannot remove '%s' from '%s'", rel, name);
	return skill_publish(ctx, skills);

err:
	return -1;
}

int fyai_skill_set_enabled(struct fyai_ctx *ctx, fy_generic names,
			   bool enabled)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic skills, skill, item;
	const char *name;

	skills = fyai_skills(ctx);
	fy_foreach(item, names) {
		name = fy_castp(&item, "");
		skill = fyai_skill_get(ctx, name);
		fyai_error_check(ctx, fy_is_valid(skill), err,
				 "no skill '%s' on branch '%s'", name,
				 fyai_ctx_branch(ctx));
		/* Enabled is the default, so the member is kept only to disable. */
		skill = enabled ? fy_disassoc(gb, skill, "enabled") :
			fy_assoc(gb, skill, "enabled", false);
		skills = fy_assoc(gb, skills, fy_value(gb, name), skill);
		fyai_error_check(ctx, fy_is_mapping(skills), err,
				 "cannot change the skill '%s'", name);
	}
	return skill_publish(ctx, skills);

err:
	return -1;
}

int fyai_skill_remove(struct fyai_ctx *ctx, fy_generic names)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic skills, item;
	const char *name;

	skills = fyai_skills(ctx);
	fy_foreach(item, names) {
		name = fy_castp(&item, "");
		fyai_error_check(ctx, fy_is_valid(fyai_skill_get(ctx, name)),
				 err, "no skill '%s' on branch '%s'",
				 name, fyai_ctx_branch(ctx));
		skills = fy_disassoc(gb, skills, fy_value(gb, name));
		fyai_error_check(ctx, fy_is_valid(skills), err,
				 "cannot remove '%s'", name);
	}
	return skill_publish(ctx, fy_is_mapping(skills) ? skills :
			     fy_map_empty);

err:
	return -1;
}

int fyai_skill_update(struct fyai_ctx *ctx, const char *name,
		      const char *file, const char *text, bool create,
		      int exec)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic skill, skills, parsed, fm, name_v, files;
	char why[1024];
	size_t len;

	skill = fyai_skill_get(ctx, name);
	fyai_error_check(ctx, !create || fy_is_invalid(skill), err,
			 "'%s' is already stored; edit it with skill edit %s",
			 name, name);
	fyai_error_check(ctx, create || fy_is_valid(skill), err,
			 "no skill '%s' on branch '%s'", name,
			 fyai_ctx_branch(ctx));
	len = strlen(text);
	fyai_error_check(ctx, len <= FYAI_SKILL_TEXT_MAX, err,
			 "the text is more than %d bytes", FYAI_SKILL_TEXT_MAX);
	if (file) {
		fyai_error_check(ctx, skill_rel_path_ok(file), err,
				 "'%s' is not a relative path under the skill",
				 file);
		fyai_error_check(ctx, !data_is_binary(text, len), err,
				 "the file '%s' is not text", file);
		files = fy_get(skill, "files", fy_map_empty);
		files = fy_is_mapping(files) ? files : fy_map_empty;
		fyai_error_check(ctx,
				 fy_is_valid(fy_get(files, file, fy_invalid)) ||
				 fy_len(files) < FYAI_SKILL_FILES_MAX, err,
				 "'%s' holds %d supporting files already", name,
				 FYAI_SKILL_FILES_MAX);
		files = fy_assoc(gb, files, fy_value(gb, file),
				 fy_value(gb, text));
		skill = fy_assoc(gb, skill, "files", files);
		if (exec >= 0)
			skill = skill_set_exec(gb, skill, file, exec);
		fyai_error_check(ctx, fy_is_mapping(skill), err,
				 "cannot keep the file '%s' of '%s'", file, name);
	} else {
		parsed = fyai_skill_parse(gb, text, name, why, sizeof(why));
		fyai_error_check(ctx, fy_is_valid(parsed), err, "%s", why);
		fm = fy_get(parsed, "frontmatter", fy_invalid);
		name_v = fy_get(fm, "name", fy_invalid);
		fyai_error_check(ctx, fy_equal(name_v, name), err,
				 "the front matter names '%s'; a skill keeps its "
				 "name '%s' (export, edit and import it to rename "
				 "it)", fy_castp(&name_v, ""), name);
		skill_import_notes(ctx, name, fm);
		skill = fy_assoc(gb, fy_is_mapping(skill) ? skill : fy_map_empty,
				 "frontmatter", fm,
				 "body", fy_get(parsed, "body", fy_invalid));
	}
	skill = fy_assoc(gb, skill, "edited",
			 (long long)fyai_branch_timestamp());
	skills = fyai_skills(ctx);
	skills = fy_assoc(gb, fy_is_mapping(skills) ? skills : fy_map_empty,
			  fy_value(gb, name), skill);
	fyai_error_check(ctx, fy_is_mapping(skills), err,
			 "cannot keep the skill '%s'", name);
	return skill_publish(ctx, skills);

err:
	return -1;
}

const char *fyai_skill_template(struct fy_generic_builder *gb,
				const char *name)
{
	return fy_gb_intern_string(gb, fy_sprintfa(
		"---\n"
		"name: %s\n"
		"description: Say what the skill does and when to use it.\n"
		"# argument-hint: \"[file]\"\n"
		"# context: fork\n"
		"---\n"
		"\n"
		"Write the instructions of the skill here. $ARGUMENTS stands for\n"
		"the arguments of the call.\n", name));
}

/* ---- presentation -------------------------------------------------------- */

static int skill_name_cmp(const void *a, const void *b)
{
	return strcmp(*(const char * const *)a, *(const char * const *)b);
}

/*
 * The names of the stored skills, sorted, so that a listing and the tool
 * description do not depend on the order of the store. The caller frees the
 * array; the names are borrowed from the store.
 */
static const char **skill_names_sorted(struct fyai_ctx *ctx, size_t *countp)
{
	fy_generic skills;
	const char **names;
	const char *key;
	size_t n;

	*countp = 0;
	skills = fyai_skills(ctx);
	if (fy_is_invalid(skills) || fy_empty(skills))
		return NULL;
	names = calloc(fy_len(skills), sizeof(*names));
	if (!names) {
		fyai_error(ctx, "cannot list the skills");
		return NULL;
	}
	n = 0;
	fy_foreach(key, skills)
		names[n++] = key;
	qsort(names, n, sizeof(*names), skill_name_cmp);
	*countp = n;
	return names;
}

fy_generic fyai_skill_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	fy_generic rows, skill, fm, files;
	const char **names;
	const char *mode;
	size_t i, n;

	rows = fy_seq_empty;
	names = skill_names_sorted(ctx, &n);
	for (i = 0; i < n; i++) {
		skill = fyai_skill_get(ctx, names[i]);
		fm = skill_frontmatter(skill);
		files = fy_get(skill, "files", fy_map_empty);
		if (!skill_fm_model(skill) && !skill_fm_user(skill))
			mode = "none";
		else if (!skill_fm_model(skill))
			mode = "user";
		else if (!skill_fm_user(skill))
			mode = "model";
		else
			mode = "both";
		rows = fy_append(gb, rows, fy_mapping(gb,
			"name", fy_value(gb, names[i]),
			"enabled", skill_enabled(skill),
			"description", fy_get(fm, "description", ""),
			"arguments", fy_get(fm, "argument-hint", ""),
			"context", fyai_skill_forks(skill) ? "fork" : "inline",
			"invocation", mode,
			"files", (long long)(fy_is_mapping(files) ?
					     fy_len(files) : 0)));
		if (fy_is_invalid(rows))
			break;
	}
	free(names);
	return rows;
}

fy_generic fyai_skill_source(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			     fy_generic skill)
{
	fy_generic fm, emitted;
	const char *text;

	(void)ctx;
	if (!fy_is_mapping(skill))
		return fy_invalid;
	fm = skill_frontmatter(skill);
	emitted = fy_emit(gb, fm, FYAI_YAML_EMIT_FLAGS |
			  FYOPEF_OUTPUT_TYPE_STRING, NULL);
	if (!fy_is_string(emitted))
		return fy_invalid;
	text = fy_castp(&emitted, "");
	return fy_stringf(gb, "---\n%s%s---\n%s", text,
			  !*text || text[strlen(text) - 1] == '\n' ? "" : "\n",
			  fy_get(skill, "body", ""));
}

/* Create @dir and each missing parent of it. */
static int skill_mkdirs(const char *dir)
{
	char path[PATH_MAX];
	char *p;

	snprintf(path, sizeof(path), "%s", dir);
	for (p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0777) && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(path, 0777) && errno != EEXIST)
		return -1;
	return 0;
}

/* Write @text to @path, which must not exist. */
/*
 * Report a failure to write a file: an error for an export, which stops, and a
 * warning for a skill written out to run, which still loads as text.
 */
#define skill_write_fail(_ctx, _warn, _fmt, ...) \
	fyai_diag_type(fyai_ctx_diag(_ctx), \
		       (_warn) ? FYAIET_WARNING : FYAIET_ERROR, \
		       (_fmt) , ## __VA_ARGS__)

/* Write @text to @path with @mode, which must not exist. */
static int skill_write_new(struct fyai_ctx *ctx, const char *path,
			   const char *text, mode_t mode, bool warn)
{
	char dir[PATH_MAX];
	char *slash;
	FILE *fp;
	size_t len;
	int fd, rc;

	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		rc = skill_mkdirs(dir);
		if (rc) {
			skill_write_fail(ctx, warn, "cannot create %s: %s", dir,
					 strerror(errno));
			return -1;
		}
	}
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
	if (fd < 0) {
		skill_write_fail(ctx, warn, "cannot create %s: %s", path,
				 strerror(errno));
		return -1;
	}
	fp = fdopen(fd, "w");
	if (!fp) {
		close(fd);
		skill_write_fail(ctx, warn, "cannot write %s: %s", path,
				 strerror(errno));
		return -1;
	}
	len = strlen(text);
	if (fwrite(text, 1, len, fp) != len) {
		fclose(fp);
		skill_write_fail(ctx, warn, "cannot write %s: %s", path,
				 strerror(errno));
		return -1;
	}
	rc = fclose(fp);
	if (rc) {
		skill_write_fail(ctx, warn, "cannot write %s: %s", path,
				 strerror(errno));
		return -1;
	}
	return 0;
}

/* Whether @rel stays under its directory: relative, with no ".." part. */
static bool skill_rel_path_ok(const char *rel)
{
	const char *p;

	if (fy_str_empty(rel) || rel[0] == '/')
		return false;
	for (p = rel; p; p = strchr(p, '/')) {
		if (*p == '/')
			p++;
		if (!strncmp(p, "..", 2) && (p[2] == '/' || !p[2]))
			return false;
	}
	return true;
}

fy_generic fyai_skill_file_rows(struct fyai_ctx *ctx,
				struct fy_generic_builder *gb, const char *name)
{
	fy_generic skill, files, key, val, rows;
	const char *text;
	long long lines;
	size_t len, i;

	skill = fyai_skill_get(ctx, name);
	fyai_error_check(ctx, fy_is_valid(skill), err,
			 "no skill '%s' on branch '%s'", name,
			 fyai_ctx_branch(ctx));
	files = fy_get(skill, "files", fy_map_empty);
	rows = fy_seq_empty;
	fy_foreach_key_value(key, val, fy_is_mapping(files) ? files :
			     fy_map_empty) {
		text = fy_castp(&val, "");
		len = strlen(text);
		lines = 0;
		for (i = 0; i < len; i++)
			lines += text[i] == '\n';
		if (len && text[len - 1] != '\n')
			lines++;
		rows = fy_append(gb, rows, fy_mapping(gb, "path", key,
			"bytes", (long long)len, "lines", lines,
			"executable",
			(_Bool)(skill_file_mode(skill, fy_castp(&key, "")) &
			       0111)));
		fyai_error_check(ctx, fy_is_sequence(rows), err,
				 "cannot list the files of '%s'", name);
	}
	return rows;

err:
	return fy_invalid;
}

int fyai_skill_export_file(struct fyai_ctx *ctx, const char *name,
			   const char *rel, const char *dir)
{
	fy_generic skill, files, text;

	skill = fyai_skill_get(ctx, name);
	fyai_error_check(ctx, fy_is_valid(skill), err,
			 "no skill '%s' on branch '%s'", name,
			 fyai_ctx_branch(ctx));
	files = fy_get(skill, "files", fy_map_empty);
	text = fy_get(fy_is_mapping(files) ? files : fy_map_empty, rel,
		      fy_invalid);
	fyai_error_check(ctx, fy_is_string(text), err,
			 "skill '%s' has no file '%s'", name, rel);
	fyai_error_check(ctx, skill_rel_path_ok(rel), err,
			 "'%s' holds the path '%s', which leaves its directory",
			 name, rel);
	return skill_write_new(ctx, fy_sprintfa("%s/%s", dir, rel),
			       fy_castp(&text, ""),
			       skill_file_mode(skill, rel), false);

err:
	return -1;
}

int fyai_skill_export(struct fyai_ctx *ctx, const char *name, const char *dir)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic skill, source, files, key, val;
	const char *rel;
	int dir_rc;

	skill = fyai_skill_get(ctx, name);
	fyai_error_check(ctx, fy_is_valid(skill), err,
			 "no skill '%s' on branch '%s'", name,
			 fyai_ctx_branch(ctx));
	source = fyai_skill_source(ctx, gb, skill);
	fyai_error_check(ctx, fy_is_string(source), err,
			 "cannot write the SKILL.md of '%s'", name);
	dir_rc = skill_mkdirs(dir);
	fyai_error_check(ctx, !dir_rc, err,
			 "cannot create %s: %s", dir, strerror(errno));
	if (skill_write_new(ctx, fy_sprintfa("%s/SKILL.md", dir),
			    fy_castp(&source, ""), 0666, false))
		return -1;
	files = fy_get(skill, "files", fy_map_empty);
	fy_foreach_key_value(key, val, files) {
		rel = fy_castp(&key, "");
		fyai_error_check(ctx, skill_rel_path_ok(rel), err,
				 "'%s' holds the path '%s', which leaves "
				 "its directory", name, rel);
		if (skill_write_new(ctx, fy_sprintfa("%s/%s", dir, rel),
				    fy_castp(&val, ""),
				    skill_file_mode(skill, rel), false))
			return -1;
	}
	return 0;

err:
	return -1;
}

/* ---- skills written out to run ------------------------------------------ */

/*
 * A skill written out in this invocation: its directory, and the stored
 * value it was written from. A changed skill is written again to a new
 * directory, as a program of the old copy can still run.
 */
struct fyai_skill_dir {
	struct fyai_skill_dir *next;
	char *name;
	char *path;
	uint64_t value;
};

static int skill_tree_remove_one(const char *path, const struct stat *st,
				 int flag, struct FTW *ftw)
{
	(void)st;
	(void)ftw;
	return flag == FTW_DP ? rmdir(path) : unlink(path);
}

/* Remove the tree at @path. A link is removed, not followed. */
static int skill_tree_remove(const char *path)
{
	return nftw(path, skill_tree_remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

void fyai_skill_dirs_release(struct fyai_ctx *ctx)
{
	if (!ctx)
		return;
	/* Cleanup cannot act on a failure: the tree is in a temporary place. */
	if (ctx->skill_root)
		(void)skill_tree_remove(ctx->skill_root);
	fyai_skill_dirs_abandon(ctx);
}

void fyai_skill_dirs_abandon(struct fyai_ctx *ctx)
{
	struct fyai_skill_dir *d;

	if (!ctx)
		return;
	while ((d = ctx->skill_dirs)) {
		ctx->skill_dirs = d->next;
		free(d->name);
		free(d->path);
		free(d);
	}
	free(ctx->skill_root);
	ctx->skill_root = NULL;
}

/* Make the private directory of this invocation under $TMPDIR. */
static int skill_root_open(struct fyai_ctx *ctx)
{
	const char *tmp;
	char tmpl[PATH_MAX];
	int rc;

	if (ctx->skill_root)
		return 0;
	tmp = getenv("TMPDIR");
	if (fy_str_empty(tmp))
		tmp = "/tmp";
	rc = snprintf(tmpl, sizeof(tmpl), "%s/fyai-skills-XXXXXX", tmp);
	if (rc < 0 || rc >= (int)sizeof(tmpl)) {
		fyai_warning(ctx, "the temporary directory %s is too long to "
			     "write skills to", tmp);
		return -1;
	}
	/* mkdtemp makes the directory 0700: no other user writes a script in. */
	if (!mkdtemp(tmpl)) {
		fyai_warning(ctx, "cannot make a directory for skills in %s: %s",
			     tmp, strerror(errno));
		return -1;
	}
	ctx->skill_root = strdup(tmpl);
	if (!ctx->skill_root) {
		(void)rmdir(tmpl);
		fyai_warning(ctx, "cannot keep the directory of the skills");
		return -1;
	}
	return 0;
}

/* Write SKILL.md and the files of @skill under the new directory @path. */
static int skill_write_out(struct fyai_ctx *ctx, fy_generic skill,
			   const char *name, const char *path)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic source, files, key, val;
	const char *rel;
	int rc;

	rc = mkdir(path, 0700);
	if (rc) {
		fyai_warning(ctx, "cannot make %s: %s", path, strerror(errno));
		return -1;
	}
	source = fyai_skill_source(ctx, gb, skill);
	if (!fy_is_string(source)) {
		fyai_warning(ctx, "cannot write the SKILL.md of '%s'", name);
		return -1;
	}
	rc = skill_write_new(ctx, fy_sprintfa("%s/SKILL.md", path),
			     fy_castp(&source, ""), 0666, true);
	if (rc)
		return -1;
	files = fy_get(skill, "files", fy_map_empty);
	fy_foreach_key_value(key, val, fy_is_mapping(files) ? files :
			     fy_map_empty) {
		rel = fy_castp(&key, "");
		if (!skill_rel_path_ok(rel)) {
			fyai_warning(ctx, "'%s' holds the path '%s', which "
				     "leaves its directory", name, rel);
			return -1;
		}
		rc = skill_write_new(ctx, fy_sprintfa("%s/%s", path, rel),
				     fy_castp(&val, ""),
				     skill_file_mode(skill, rel), true);
		if (rc)
			return -1;
	}
	return 0;
}

/*
 * The directory that @skill is written out to in this invocation, made the
 * first time it is asked for, in the process that asks: a view gives each
 * agent a /tmp of its own, so only the process that runs the programs of a
 * skill can write the files those programs see. NULL for a skill with no
 * files, and with a warning when the files cannot be written; the files
 * are then read through the skill tool alone.
 */
static const char *skill_dir(struct fyai_ctx *ctx, fy_generic skill)
{
	struct fyai_skill_dir *d;
	fy_generic files, name_v;
	const char *name;
	char path[PATH_MAX];
	unsigned int n;
	int rc;

	files = fy_get(skill, "files", fy_invalid);
	if (!ctx || !fy_is_mapping(files) || fy_empty(files))
		return NULL;
	name_v = fy_get(skill_frontmatter(skill), "name", fy_invalid);
	name = fy_castp(&name_v, "");
	n = 0;
	for (d = ctx->skill_dirs; d; d = d->next) {
		if (strcmp(d->name, name))
			continue;
		if (d->value == skill.v)
			return d->path;
		n++;
	}
	if (skill_root_open(ctx))
		return NULL;
	rc = n ? snprintf(path, sizeof(path), "%s/%s.%u", ctx->skill_root,
			  name, n + 1) :
		 snprintf(path, sizeof(path), "%s/%s", ctx->skill_root, name);
	if (rc < 0 || rc >= (int)sizeof(path)) {
		fyai_warning(ctx, "the directory of skill '%s' is too long",
			     name);
		return NULL;
	}
	rc = skill_write_out(ctx, skill, name, path);
	if (rc) {
		(void)skill_tree_remove(path);
		fyai_warning(ctx, "the files of skill '%s' are not written "
			     "out; the model reads them through the skill tool",
			     name);
		return NULL;
	}
	d = calloc(1, sizeof(*d));
	if (d) {
		d->name = strdup(name);
		d->path = strdup(path);
	}
	if (!d || !d->name || !d->path) {
		if (d) {
			free(d->name);
			free(d->path);
			free(d);
		}
		(void)skill_tree_remove(path);
		fyai_warning(ctx, "cannot keep the directory of skill '%s'",
			     name);
		return NULL;
	}
	d->value = skill.v;
	d->next = ctx->skill_dirs;
	ctx->skill_dirs = d;
	return d->path;
}

/* ---- arguments ----------------------------------------------------------- */

/*
 * Split @args into words as a shell does for quotes: a blank separates two
 * words, and '...' or "..." keeps blanks in one. A sequence built in @gb.
 */
static fy_generic skill_split_args(struct fy_generic_builder *gb,
				   const char *args)
{
	struct fyai_buf word = {0};
	fy_generic words;
	bool in_word;
	char quote;
	const char *p;

	words = fy_seq_empty;
	in_word = false;
	quote = 0;
	for (p = args ? args : ""; ; p++) {
		if (*p && (quote ? *p != quote :
			   (*p != ' ' && *p != '\t' && *p != '\n'))) {
			if (!quote && (*p == '\'' || *p == '"')) {
				quote = *p;
				in_word = true;
				continue;
			}
			if (fyai_buf_append_data(&word, p, 1))
				goto err;
			in_word = true;
			continue;
		}
		if (*p && quote) {
			quote = 0;
			continue;
		}
		if (in_word) {
			words = fy_append(gb, words, fy_value(gb,
				word.data ? word.data : ""));
			free(word.data);
			memset(&word, 0, sizeof(word));
			in_word = false;
		}
		if (!*p)
			break;
	}
	return words;

err:
	free(word.data);
	return fy_invalid;
}

fy_generic fyai_skill_expand(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			     fy_generic skill, const char *args)
{
	struct fyai_buf out = {0};
	fy_generic body_v, fm, name_v, words, w;
	const char *body, *p, *name, *dir;
	bool named;
	char *end;
	long idx;
	int rc;

	args = args ? args : "";
	body_v = fy_get(skill, "body", fy_invalid);
	body = fy_castp(&body_v, "");
	fm = skill_frontmatter(skill);
	name_v = fy_get(fm, "name", fy_invalid);
	name = fy_castp(&name_v, "");
	words = skill_split_args(gb, args);
	if (fy_is_invalid(words))
		return fy_invalid;
	/* The files are where the programs of the skill can open them. */
	dir = skill_dir(ctx, skill);
	named = false;
	rc = 0;
	for (p = body; *p && !rc; ) {
		if (!strncmp(p, "$ARGUMENTS[", 11)) {
			idx = strtol(p + 11, &end, 10);
			if (end != p + 11 && *end == ']') {
				w = fy_get_at(words, (size_t)idx);
				rc = fyai_buf_append(&out,
					fy_is_string(w) ? fy_castp(&w, "") : "");
				named = true;
				p = end + 1;
				continue;
			}
		}
		if (!strncmp(p, "$ARGUMENTS", 10)) {
			rc = fyai_buf_append(&out, args);
			named = true;
			p += 10;
			continue;
		}
		if (p[0] == '$' && p[1] >= '0' && p[1] <= '9') {
			idx = strtol(p + 1, &end, 10);
			w = fy_get_at(words, (size_t)idx);
			rc = fyai_buf_append(&out,
				fy_is_string(w) ? fy_castp(&w, "") : "");
			named = true;
			p = end;
			continue;
		}
		if (!strncmp(p, "${CLAUDE_SKILL_DIR}", 19)) {
			rc = fyai_buf_append(&out, dir ? dir :
				fy_sprintfa("skill:%s", name));
			p += 19;
			continue;
		}
		if (!strncmp(p, "${CLAUDE_SESSION_ID}", 20)) {
			rc = fyai_buf_append(&out, fyai_ctx_branch(ctx));
			p += 20;
			continue;
		}
		rc = fyai_buf_append_data(&out, p, 1);
		p++;
	}
	if (!rc && !named && *args)
		rc = fyai_buf_append(&out,
			fy_sprintfa("%sARGUMENTS: %s\n",
				    out.len && out.data[out.len - 1] != '\n' ?
				    "\n\n" : "\n", args));
	if (rc) {
		free(out.data);
		return fy_invalid;
	}
	w = fy_value(gb, out.data ? out.data : "");
	w = fy_gb_internalize(gb, w);
	free(out.data);
	return w;
}

/* The supporting files of @skill as "a, b", or "". In @gb. */
static const char *skill_file_list(struct fy_generic_builder *gb,
				   fy_generic skill)
{
	fy_generic files, key, list;

	files = fy_get(skill, "files", fy_map_empty);
	list = fy_seq_empty;
	fy_foreach(key, fy_is_mapping(files) ? files : fy_map_empty)
		list = fy_append(gb, list, key);
	if (fy_empty(list))
		return "";
	list = fyai_skill_join(gb, list, ", ");
	return fy_gb_intern_string(gb, fy_castp(&list, ""));
}

/* The text that loads @skill into the conversation, built in @gb. */
static fy_generic skill_loaded_text(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb,
				    fy_generic skill, const char *args)
{
	fy_generic body, name_v;
	const char *files, *dir, *name;

	name_v = fy_get(skill_frontmatter(skill), "name", fy_invalid);
	name = fy_castp(&name_v, "");
	body = fyai_skill_expand(ctx, gb, skill, args);
	if (fy_is_invalid(body))
		return fy_invalid;
	files = skill_file_list(gb, skill);
	/* The expansion wrote the files out; this finds the same directory. */
	dir = skill_dir(ctx, skill);
	if (dir)
		return fy_stringf(gb,
			"Skill `%s` is loaded. Follow its instructions for this "
			"task.\nBase directory for this skill: %s\nA relative "
			"path in the skill, such as `scripts/...`, is under that "
			"directory; run its programs from there. Its files: %s\n"
			"\n%s", name, dir, files, fy_castp(&body, ""));
	return fy_stringf(gb,
		"Skill `%s` is loaded. Follow its instructions for this task.\n"
		"%s%s%s\n%s", name,
		*files ? "Its supporting files are under `skill:" : "",
		*files ? fy_sprintfa("%s/`; read one with the `skill` tool "
				     "and `file`: %s\n", name, files) : "",
		"", fy_castp(&body, ""));
}

fy_generic fyai_skill_task(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			   const char *name, const char *args)
{
	fy_generic skill;

	skill = fyai_skill_get(ctx, name);
	fyai_error_check(ctx, fy_is_valid(skill) && skill_model_invocable(skill),
			 err, "no enabled skill '%s' on branch '%s'", name,
			 fyai_ctx_branch(ctx));
	return skill_loaded_text(ctx, gb, skill, args);
err:
	return fy_invalid;
}

/* The line that runs a skill whose name a command takes: "/skill exec NAME ARGS". */
#define SKILL_EXEC_PREFIX	"/skill exec"

/*
 * The skill that @line runs, or fy_invalid: "/NAME ARGS", or
 * "/skill exec NAME ARGS" for a skill whose name a command of the session
 * takes. NAME goes to @name and *@argsp points at ARGS.
 */
static fy_generic skill_of_line(struct fyai_ctx *ctx, const char *line,
				char *name, size_t name_size,
				const char **argsp)
{
	fy_generic skill;
	const char *word;
	size_t len;
	bool exec;

	if (!line || line[0] != '/' || line[1] == '/')
		return fy_invalid;
	exec = fyai_skill_exec_line(line);
	word = exec ? line + strlen(SKILL_EXEC_PREFIX) : line + 1;
	word += strspn(word, " \t");
	len = strcspn(word, " \t\n");
	if (!len || len >= name_size)
		return fy_invalid;
	memcpy(name, word, len);
	name[len] = '\0';
	/* A command of the session takes /NAME before a skill does. */
	if (!exec && fyai_cmd_session_exact(name, len))
		return fy_invalid;
	skill = fyai_skill_get(ctx, name);
	if (fy_is_invalid(skill) || !skill_user_invocable(skill))
		return fy_invalid;
	*argsp = word + len + strspn(word + len, " \t");
	return skill;
}

bool fyai_skill_exec_line(const char *line)
{
	size_t n = strlen(SKILL_EXEC_PREFIX);

	return line && !strncmp(line, SKILL_EXEC_PREFIX, n) &&
	       (line[n] == ' ' || line[n] == '\t');
}

bool fyai_skill_user_line(struct fyai_ctx *ctx, const char *line)
{
	char name[FYAI_SKILL_NAME_MAX + 1];
	const char *args;

	return fy_is_valid(skill_of_line(ctx, line, name, sizeof(name), &args));
}

fy_generic fyai_skill_user_message(struct fyai_ctx *ctx,
				   struct fy_generic_builder *gb,
				   const char *line)
{
	fy_generic skill, text;
	char name[FYAI_SKILL_NAME_MAX + 1];
	const char *args;

	skill = skill_of_line(ctx, line, name, sizeof(name), &args);
	if (fy_is_invalid(skill))
		return fy_invalid;
	if (fyai_skill_forks(skill))
		return fy_stringf(gb,
			"The user ran the skill `%s`%s%s. It runs as a "
			"sub-agent: call the `skill` tool with skill `%s`%s%s%s, "
			"then report the result.",
			name, *args ? " with the arguments: " : "", args,
			name, *args ? " and args `" : "", args,
			*args ? "`" : "");
	text = skill_loaded_text(ctx, gb, skill, args);
	if (fy_is_invalid(text))
		return fy_invalid;
	return fy_stringf(gb, "The user ran the skill `%s`%s%s.\n\n%s", name,
			  *args ? " with the arguments: " : "", args,
			  fy_castp(&text, ""));
}

fy_generic fyai_skill_user_names(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb)
{
	fy_generic out, skill, fm;
	const char **names;
	size_t i, n;

	out = fy_seq_empty;
	names = skill_names_sorted(ctx, &n);
	for (i = 0; i < n; i++) {
		skill = fyai_skill_get(ctx, names[i]);
		if (!skill_user_invocable(skill))
			continue;
		fm = skill_frontmatter(skill);
		out = fy_append(gb, out, fy_mapping(gb,
			"name", fy_value(gb, names[i]),
			"description", fy_get(fm, "description", ""),
			"hint", fy_get(fm, "argument-hint", "")));
	}
	free(names);
	return out;
}

/* ---- tools of a forked skill --------------------------------------------- */

/* Claude Code tool names and the fyai tools that do their work. */
static const struct {
	const char *claude;
	const char *fyai;
} skill_tool_map[] = {
	{ "Read",		"read_file" },
	{ "Write",		"write_file" },
	{ "Edit",		"apply_patch" },
	{ "MultiEdit",		"apply_patch" },
	{ "Bash",		"shell" },
	{ "Glob",		"shell" },
	{ "Grep",		"shell" },
	{ "LS",			"shell" },
	{ "Task",		"agent" },
	{ "Agent",		"agent" },
	{ "TodoWrite",		"todo_write" },
	{ "AskUserQuestion",	"ask_user" },
	{ "Skill",		"skill" },
};

/* The tools a shell brings with it: a session is driven through them. */
static const char * const skill_shell_tools[] = {
	"shell_input", "shell_output", "shell_close",
};

/* Tools that every forked skill keeps: its files, its questions, the time. */
static const char * const skill_base_tools[] = {
	"skill", "ask_parent", "time",
};

/* Append @name to @list unless it is there. */
static fy_generic skill_tools_add(struct fy_generic_builder *gb,
				  fy_generic list, const char *name)
{
	fy_generic item;

	fy_foreach(item, list)
		if (fy_equal(item, name))
			return list;
	return fy_append(gb, list, fy_value(gb, name));
}

/* Map one name of allowed-tools, such as "Bash(git:*)", to a fyai tool. */
static const char *skill_tool_map_one(struct fy_generic_builder *gb,
				      const char *word)
{
	const char *paren, *name;
	size_t i;

	paren = strchr(word, '(');
	name = paren ? fy_gb_intern_string(gb,
			fy_sprintfa("%.*s", (int)(paren - word), word)) : word;
	for (i = 0; i < ARRAY_SIZE(skill_tool_map); i++)
		if (!strcmp(name, skill_tool_map[i].claude))
			return skill_tool_map[i].fyai;
	name = fyai_tool_name_canonical(name);
	return fyai_tool_find(name) ? name : NULL;
}

fy_generic fyai_skill_allowed_tools(struct fy_generic_builder *gb,
				    fy_generic frontmatter,
				    fy_generic *dropped)
{
	fy_generic allowed, words, item, list;
	const char *word, *tool;
	char *copy, *tok, *save;
	size_t i;

	allowed = fy_get(frontmatter, "allowed-tools", fy_invalid);
	if (fy_is_string(allowed)) {
		/* A string holds names separated by commas or blanks. */
		words = fy_seq_empty;
		copy = strdup(fy_castp(&allowed, ""));
		if (!copy)
			return fy_invalid;
		for (tok = strtok_r(copy, ", \t\n", &save); tok;
		     tok = strtok_r(NULL, ", \t\n", &save))
			words = fy_append(gb, words, fy_value(gb, tok));
		free(copy);
	} else if (fy_is_sequence(allowed)) {
		words = allowed;
	} else {
		return fy_invalid;
	}
	list = fy_seq_empty;
	fy_foreach(item, words) {
		word = fy_castp(&item, "");
		tool = skill_tool_map_one(gb, word);
		if (!tool) {
			if (dropped)
				*dropped = fy_append(gb, *dropped,
						     fy_value(gb, word));
			continue;
		}
		list = skill_tools_add(gb, list, tool);
		if (!strcmp(tool, "shell"))
			for (i = 0; i < ARRAY_SIZE(skill_shell_tools); i++)
				list = skill_tools_add(gb, list,
						       skill_shell_tools[i]);
	}
	for (i = 0; i < ARRAY_SIZE(skill_base_tools); i++)
		list = skill_tools_add(gb, list, skill_base_tools[i]);
	return list;
}

bool fyai_skill_tool_allowed(struct fyai_ctx *ctx, const char *name)
{
	fy_generic item;

	/*
	 * An allow list is never empty: it keeps the base tools. A zeroed
	 * context field reads as an empty sequence and restricts nothing.
	 */
	if (!ctx || !fy_is_sequence(ctx->tools_allow) ||
	    fy_empty(ctx->tools_allow))
		return true;
	fy_foreach(item, ctx->tools_allow)
		if (fy_equal(item, name))
			return true;
	return false;
}

/* ---- the skill tool ------------------------------------------------------ */

/* The names that the model can load, as "a, b", for a cause. In @gb. */
static const char *skill_model_names(struct fyai_ctx *ctx,
				     struct fy_generic_builder *gb)
{
	fy_generic list;
	const char **names;
	size_t i, n;

	list = fy_seq_empty;
	names = skill_names_sorted(ctx, &n);
	for (i = 0; i < n; i++)
		if (skill_model_invocable(fyai_skill_get(ctx, names[i])))
			list = fy_append(gb, list, fy_value(gb, names[i]));
	free(names);
	if (fy_empty(list))
		return "none";
	list = fyai_skill_join(gb, list, ", ");
	return fy_gb_intern_string(gb, fy_castp(&list, "none"));
}

static fy_generic tool_skill(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic name_v, args_v, file_v, skill, text, files;
	const char *name, *file;

	*okp = false;
	name_v = fy_get(args, "skill", fy_invalid);
	args_v = fy_get(args, "args", fy_invalid);
	file_v = fy_get(args, "file", fy_invalid);
	name = fy_castp(&name_v, "");
	/* A name given as the user types it keeps its slash. */
	if (*name == '/')
		name++;
	skill = fyai_skill_get(ctx, name);
	if (fy_is_invalid(skill) || !skill_model_invocable(skill))
		return fy_stringf(gb, "tool error: no skill '%s'; the skills "
				  "are: %s", name, skill_model_names(ctx, gb));
	if (fy_is_string(file_v)) {
		file = fy_castp(&file_v, "");
		if (!strncmp(file, fy_sprintfa("skill:%s/", name),
			     strlen(name) + 7))
			file += strlen(name) + 7;
		files = fy_get(skill, "files", fy_map_empty);
		text = fy_get(fy_is_mapping(files) ? files : fy_map_empty,
			      file, fy_invalid);
		if (!fy_is_string(text))
			return fy_stringf(gb, "tool error: skill '%s' has no "
					  "file '%s'; its files are: %s", name,
					  file, *skill_file_list(gb, skill) ?
					  skill_file_list(gb, skill) : "none");
		*okp = true;
		return text;
	}
	/*
	 * A forked skill runs as an agent call, which the scheduler makes of
	 * this call. It arrives here only where no sub-agent can start.
	 */
	if (fyai_skill_forks(skill))
		return fy_stringf(gb, "tool error: skill '%s' runs as a "
				  "sub-agent, and this agent cannot start one",
				  name);
	text = skill_loaded_text(ctx, gb, skill,
				 fy_is_string(args_v) ? fy_castp(&args_v, "") :
				 "");
	if (fy_is_invalid(text))
		return fy_value(gb, "tool error: the skill cannot be loaded");
	*okp = true;
	return text;
}

static void tool_head_skill(struct fyai_ctx *ctx, FILE *mf,
			    struct fy_generic_builder *gb, fy_generic args,
			    int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic name, a, file;

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	name = fy_get(args, "skill", fy_invalid);
	a = fy_get(args, "args", fy_invalid);
	file = fy_get(args, "file", fy_invalid);
	fprintf(mf, "**skill** %s", fy_castp(&name, ""));
	if (fy_is_string(file))
		fprintf(mf, " %s", fy_castp(&file, ""));
	else if (fy_is_string(a) && !fy_empty(a))
		fprintf(mf, " %s", fy_castp(&a, ""));
	fprintf(mf, "\n\n");
}

/*
 * List the skills that the model can load in the description of the tool, as
 * the agent tool lists its personas. A run with none does not offer the tool.
 */
static fy_generic tool_describe_skill(struct fyai_ctx *ctx,
				      struct fy_generic_builder *gb,
				      fy_generic tool)
{
	struct fyai_buf list = {0};
	fy_generic fn, skill, fm, desc, when, hint;
	const char **names;
	size_t i, n;
	int rc;

	names = skill_names_sorted(ctx, &n);
	rc = 0;
	for (i = 0; i < n && !rc; i++) {
		skill = fyai_skill_get(ctx, names[i]);
		if (!skill_model_invocable(skill))
			continue;
		fm = skill_frontmatter(skill);
		desc = fy_get(fm, "description", fy_invalid);
		when = fy_get(fm, "when_to_use", fy_invalid);
		hint = fy_get(fm, "argument-hint", fy_invalid);
		rc = fyai_buf_append(&list, fy_sprintfa(
			"\n- `%s`%s%s%s: %s%s%s", names[i],
			fy_is_string(hint) ? " (arguments: " : "",
			fy_is_string(hint) ? fy_castp(&hint, "") : "",
			fy_is_string(hint) ? ")" : "",
			fy_castp(&desc, ""),
			fy_is_string(when) ? " " : "",
			fy_is_string(when) ? fy_castp(&when, "") : ""));
	}
	free(names);
	if (rc || !list.data) {
		free(list.data);
		return rc ? tool : fy_invalid;
	}
	fn = fy_get(tool, "function");
	desc = fy_get(fn, "description", fy_invalid);
	fn = fy_assoc(gb, fn, "description",
		fy_stringf(gb, "%s\n\nThe skills:%s", fy_castp(&desc, ""),
			   list.data));
	free(list.data);
	return fy_assoc(gb, tool, "function", fn);
}

/* A short stable tag of @call_id, so two calls of one skill differ. */
static unsigned int skill_call_tag(const char *call_id)
{
	unsigned int h = 2166136261u;

	for (; call_id && *call_id; call_id++)
		h = (h ^ (unsigned char)*call_id) * 16777619u;
	return h & 0xffffff;
}

/*
 * A forked skill runs as a call of the agent tool: a fresh sub-agent whose
 * task is the expanded skill, with the persona, model, tools, isolation and
 * time limit of the skill. A run that cannot start a sub-agent keeps the
 * call, and the skill tool says why.
 */
static bool tool_expand_skill(struct fyai_ctx *ctx, fy_generic args,
			      const char *call_id, const char **namep,
			      fy_generic *argsp)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic name_v, args_v, skill, fm, task, out, tools, v, extra;
	const char *name;
	char agent[SKILL_AGENT_NAME_MAX + 1];

	if (fy_is_valid(fy_get(args, "file", fy_invalid)))
		return false;
	name_v = fy_get(args, "skill", fy_invalid);
	name = fy_castp(&name_v, "");
	if (*name == '/')
		name++;
	skill = fyai_skill_get(ctx, name);
	if (fy_is_invalid(skill) || !skill_model_invocable(skill) ||
	    !fyai_skill_forks(skill))
		return false;
	/* The sub-agent tools must be offered to this run. */
	if (fyai_tool_has("agent", FYAI_TOOL_NOT_FOR_CHILD) &&
	    ctx->cfg->agent_child && !ctx->agent_execution)
		return false;
	if (!fyai_skill_tool_allowed(ctx, "agent"))
		return false;
	fm = skill_frontmatter(skill);
	args_v = fy_get(args, "args", fy_invalid);
	/*
	 * The sub-agent loads the skill itself: it can run in a view, whose
	 * /tmp this process does not see, so only it can write the files out
	 * where its programs find them. The task here names the call.
	 */
	task = fy_stringf(gb, "Run the skill `%s`.", name);
	snprintf(agent, sizeof(agent), "skill-%.*s-%06x",
		 (int)(SKILL_AGENT_NAME_MAX - 13), name,
		 skill_call_tag(call_id));
	out = fy_mapping(gb,
		"name", fy_value(gb, agent),
		"description", fy_stringf(gb, "skill %s", name),
		"task", task,
		"context", "fresh");
	v = fy_get(fm, "agent", fy_invalid);
	if (fy_is_string(v))
		out = fy_assoc(gb, out, "persona", v);
	v = fy_get(fm, "isolated", fy_invalid);
	if (fy_generic_is_bool(v))
		out = fy_assoc(gb, out, "isolated", v);
	v = fy_get(fm, "background", fy_invalid);
	if (fy_generic_is_bool(v))
		out = fy_assoc(gb, out, "background", v);
	v = fy_get(fm, "timeout", fy_invalid);
	if (fy_generic_is_int(v))
		out = fy_assoc(gb, out, "timeout", v);
	/* What the agent tool schema has no parameter for. */
	extra = fy_mapping(gb, "name", fy_value(gb, name),
			   "args", fy_is_string(args_v) ? args_v :
					fy_value(gb, ""));
	v = fy_get(fm, "model", fy_invalid);
	if (fy_is_string(v))
		extra = fy_assoc(gb, extra, "model", v);
	tools = fyai_skill_allowed_tools(gb, fm, NULL);
	if (fy_is_sequence(tools))
		extra = fy_assoc(gb, extra, "tools", tools);
	out = fy_assoc(gb, out, "_fyai_skill", extra);
	if (fy_is_invalid(out))
		return false;
	*namep = "agent";
	*argsp = out;
	return true;
}

const struct fyai_tool_def fyai_skill_defs[] = {
	{ .name = "skill", .run = tool_skill, .head = tool_head_skill,
	  .describe = tool_describe_skill, .expand = tool_expand_skill,
	  .flags = FYAI_TOOL_PARENT },
};
const size_t fyai_skill_defs_count = ARRAY_SIZE(fyai_skill_defs);
