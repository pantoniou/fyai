/*
 * fyai_cmd_skill.c - the skill commands
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_SKILL

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_config.h"
#include "fyai_event.h"
#include "fyai_sink.h"
#include "fyai_skill.h"
#include "fyai_ui.h"
#include "utils.h"

int fyai_cmd_skill_list(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_skill_rows(call->ctx, call->gb);
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "cannot list the skills");
	return 0;
err:
	return -1;
}

/* The language of a fenced block for a supporting file, by its extension. */
static const char *skill_file_lang(const char *file)
{
	static const struct {
		const char *ext;
		const char *lang;
	} langs[] = {
		{ "py", "python" }, { "sh", "bash" }, { "bash", "bash" },
		{ "js", "javascript" }, { "mjs", "javascript" },
		{ "ts", "typescript" }, { "json", "json" }, { "yaml", "yaml" },
		{ "yml", "yaml" }, { "toml", "toml" }, { "xml", "xml" },
		{ "xsd", "xml" }, { "html", "html" }, { "css", "css" },
		{ "c", "c" }, { "h", "c" }, { "rs", "rust" }, { "go", "go" },
	};
	const char *dot, *slash;
	size_t i;

	dot = strrchr(file, '.');
	slash = strrchr(file, '/');
	if (!dot || (slash && dot < slash))
		return "text";
	for (i = 0; i < ARRAY_SIZE(langs); i++)
		if (!strcmp(dot + 1, langs[i].ext))
			return langs[i].lang;
	return "text";
}

/*
 * Escape each '$' of the Markdown @text that is not in code, so the renderer
 * does not read "$ARGUMENTS ... $0" as a math span: a skill names its
 * arguments with '$', and a SKILL.md is not LaTeX. A '$' in a fenced block
 * or a code span is literal already, and a backslash there would show.
 * Built in @gb.
 */
static fy_generic skill_md_escape_dollar(struct fy_generic_builder *gb,
					 const char *text)
{
	struct fyai_buf out = {0};
	const char *line, *eol, *p, *q, *close;
	char fence;
	size_t fence_len, run, n, ind;
	fy_generic v;
	int rc;

	rc = 0;
	fence = 0;
	fence_len = 0;
	for (line = text; *line && !rc; line = *eol ? eol + 1 : eol) {
		eol = strchr(line, '\n');
		if (!eol)
			eol = line + strlen(line);
		ind = strspn(line, " ");
		p = line + ind;
		n = (p[0] == '`' || p[0] == '~') ? strspn(p, p[0] == '`' ?
							   "`" : "~") : 0;
		/* A fence opens and closes a block that is copied as it is. */
		if (ind < 4 && n >= 3 &&
		    (!fence || (p[0] == fence && n >= fence_len))) {
			fence = fence ? 0 : p[0];
			fence_len = n;
		} else if (!fence) {
			for (p = line; p < eol && !rc; p++) {
				if (*p == '`') {
					run = strspn(p, "`");
					for (close = p + run; close < eol; close++)
						if (*close == '`' &&
						    strspn(close, "`") == run)
							break;
					q = close < eol ? close + run : p + run;
					rc = fyai_buf_append_data(&out, p,
							(size_t)(q - p));
					p = q - 1;
					continue;
				}
				if (*p == '$' && (p == line || p[-1] != '\\'))
					rc = fyai_buf_append(&out, "\\");
				rc = rc || fyai_buf_append_data(&out, p, 1);
			}
			if (!rc && *eol)
				rc = fyai_buf_append(&out, "\n");
			continue;
		}
		rc = fyai_buf_append_data(&out, line,
				(size_t)(eol - line) + (*eol ? 1 : 0));
	}
	v = rc ? fy_invalid : fy_value(gb, out.data ? out.data : "");
	v = fy_gb_internalize(gb, v);
	free(out.data);
	return v;
}

/* The body as the render shows it: escaped for Markdown, else as it stands. */
static fy_generic skill_show_body(struct fyai_cmd_call *call, fy_generic body)
{
	if (call->format != FYAI_CMD_OUT_MARKDOWN || !call->ctx->cfg->markdown)
		return body;
	return skill_md_escape_dollar(call->gb, fy_castp(&body, ""));
}

/*
 * Show a skill as its parts: the front matter as YAML and the body as
 * Markdown. A supporting file is Markdown, or code in the language of its
 * extension. The render puts each part in its own form.
 */
int fyai_cmd_skill_show(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *file = fyai_cmd_arg_str(call, "file");
	fy_generic skill, files, text, fm, emitted, body;
	size_t len;

	skill = fyai_skill_get(call->ctx, name);
	fyai_error_check(call->ctx, fy_is_valid(skill), err,
			 "no skill '%s' on branch '%s'", name,
			 fyai_ctx_branch(call->ctx));
	if (!file) {
		fm = fy_get(skill, "frontmatter", fy_map_empty);
		emitted = fy_emit(call->gb, fm, FYAI_YAML_EMIT_FLAGS |
				  FYOPEF_OUTPUT_TYPE_STRING |
				  FYOPEF_NO_ENDING_NEWLINE, NULL);
		fyai_error_check(call->ctx, fy_is_string(emitted), err,
				 "cannot write the front matter of '%s'", name);
		body = skill_show_body(call, fy_get(skill, "body", fy_invalid));
		fyai_error_check(call->ctx, fy_is_string(body), err,
				 "cannot show the body of '%s'", name);
		*result = fy_mapping(call->gb, "name", name, "file", "SKILL.md",
				     "frontmatter", emitted, "body", body);
		files = fy_get(skill, "files", fy_invalid);
		if (fy_is_mapping(files) && !fy_empty(files))
			*result = fy_assoc(call->gb, *result, "files",
					   fyai_skill_join(call->gb,
						fy_keys(call->gb, files), "`, `"));
		return 0;
	}
	files = fy_get(skill, "files", fy_map_empty);
	text = fy_get(fy_is_mapping(files) ? files : fy_map_empty, file,
		      fy_invalid);
	fyai_error_check(call->ctx, fy_is_string(text), err,
			 "skill '%s' has no file '%s'", name, file);
	len = strlen(file);
	if (len > 3 && !strcmp(file + len - 3, ".md")) {
		body = skill_show_body(call, text);
		fyai_error_check(call->ctx, fy_is_string(body), err,
				 "cannot show '%s' of '%s'", file, name);
		*result = fy_mapping(call->gb, "name", name, "file", file,
				     "body", body);
	}
	else
		*result = fy_mapping(call->gb, "name", name, "file", file,
				     "code", text,
				     "lang", skill_file_lang(file));
	return 0;
err:
	return -1;
}

int fyai_cmd_skill_import(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *path = fyai_cmd_arg_str(call, "path");
	const char *into = fyai_cmd_arg_str(call, "into");
	const char *as = fyai_cmd_arg_str(call, "as");
	const char *claude = fyai_cmd_arg_str(call, "claude");
	fy_generic names;

	fyai_error_check(call->ctx, !claude || (!path && !into), err,
			 "--claude finds the skills itself; it takes no PATH "
			 "and no --into");
	if (claude) {
		if (fyai_skill_import_claude(call->ctx, call->gb, claude,
					     fyai_cmd_arg_bool(call, "force"),
					     &names))
			return -1;
		*result = fy_mapping(call->gb, "path", claude,
				     "names", fy_empty(names) ?
					fy_value(call->gb, "nothing new") :
					fyai_skill_join(call->gb, names, ", "),
				     "count", (long long)fy_len(names));
		return 0;
	}
	fyai_error_check(call->ctx, path, err,
			 "name the PATH to import, or give --claude");

	fyai_error_check(call->ctx, !as || into, err,
			 "--as names a supporting file; give the skill with "
			 "--into");
	if (into) {
		if (fyai_skill_import_file(call->ctx, into, path, as))
			return -1;
		*result = fy_mapping(call->gb, "path", path, "names",
				     fy_stringf(call->gb, "%s into %s",
						as ? as : path, into));
		return 0;
	}
	if (fyai_skill_import(call->ctx, call->gb, path,
			      fyai_cmd_arg_bool(call, "force"), &names))
		return -1;
	*result = fy_mapping(call->gb, "path", path,
			     "names", fyai_skill_join(call->gb, names, ", "),
			     "count", (long long)fy_len(names));
	return 0;
err:
	return -1;
}

int fyai_cmd_skill_remove(struct fyai_cmd_call *call, fy_generic *result)
{
	fy_generic names = fy_get(call->args, "names", fy_seq_empty);
	const char *file = fyai_cmd_arg_str(call, "file");
	fy_generic name;

	if (file) {
		fyai_error_check(call->ctx, fy_len(names) == 1, err,
				 "--file removes a file of one skill");
		name = fy_get_at(names, 0);
		if (fyai_skill_remove_file(call->ctx, fy_castp(&name, ""), file))
			return -1;
		*result = fy_mapping(call->gb, "names",
				     fy_stringf(call->gb, "%s from %s", file,
						fy_castp(&name, "")));
		return 0;
	}
	if (fyai_skill_remove(call->ctx, names))
		return -1;
	*result = fy_mapping(call->gb, "names",
			     fyai_skill_join(call->gb, names, ", "));
	return 0;
err:
	return -1;
}

int fyai_cmd_skill_export(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *dir = fyai_cmd_arg_str(call, "dir");
	const char *file = fyai_cmd_arg_str(call, "file");
	fy_generic skill, files, text;

	fyai_error_check(call->ctx, file || strcmp(dir, "-"), err,
			 "standard output takes one file; give --file");
	if (file && !strcmp(dir, "-")) {
		skill = fyai_skill_get(call->ctx, name);
		files = fy_get(skill, "files", fy_map_empty);
		text = fy_get(fy_is_mapping(files) ? files : fy_map_empty,
			      file, fy_invalid);
		fyai_error_check(call->ctx, fy_is_string(text), err,
				 "skill '%s' has no file '%s'", name, file);
		/* The file as it stands, for another program to read. */
		(void)fyai_sink_write(call->ctx->sink, FYAI_SINK_MACHINE,
				      fy_castp(&text, ""), fy_len(text));
		*result = fy_null;
		return 0;
	}
	if (file ? fyai_skill_export_file(call->ctx, name, file, dir) :
		   fyai_skill_export(call->ctx, name, dir))
		return -1;
	*result = fy_mapping(call->gb, "name", name, "dir", dir,
			     "what", file ? fy_stringf(call->gb, "%s of %s",
						       file, name) :
					    fy_value(call->gb, name));
	if (file)
		*result = fy_assoc(call->gb, *result, "file", file);
	return 0;
err:
	return -1;
}

int fyai_cmd_skill_files(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_skill_file_rows(call->ctx, call->gb,
				       fyai_cmd_arg_str(call, "name"));
	return fy_is_valid(*result) ? 0 : -1;
}

/*
 * An edit of a skill: the text goes to a file, the editor runs on it in the
 * work pane or on the terminal, and the edited text is checked and stored
 * when the editor ends. A text that fails the check stays in its file, which
 * the cause names.
 */
struct skill_edit {
	struct fyai_cmd_call *call;
	struct fyai_editor_request *editor;
	char *name;
	char *file;		/* the supporting file, or NULL for SKILL.md */
	char *path;		/* the file the editor runs on */
	bool create;
	bool ui_external;
};

static void skill_edit_cleanup(struct fyai_cmd_call *call)
{
	struct skill_edit *e = call->priv;

	if (!e)
		return;
	fyai_editor_destroy(e->editor);
	if (e->ui_external)
		(void)fyai_ui_external_end(call->ctx);
	free(e->name);
	free(e->file);
	free(e->path);
	free(e);
	call->priv = NULL;
}

static void skill_edit_cancel(struct fyai_cmd_call *call)
{
	struct skill_edit *e = call->priv;

	if (e && e->editor)
		fyai_editor_cancel(e->editor);
}

/* Check and store the edited text. Runs deferred, outside the editor callback. */
static void skill_edit_service(void *userdata)
{
	struct skill_edit *e = userdata;
	struct fyai_cmd_call *call = e->call;
	struct fyai_ctx *ctx = call->ctx;
	char *text;
	int rc;

	rc = fyai_editor_collect(e->editor);
	fyai_editor_destroy(e->editor);
	e->editor = NULL;
	if (e->ui_external) {
		if (fyai_ui_external_end(ctx))
			rc = -1;
		e->ui_external = false;
	}
	if (rc) {
		fyai_error(ctx, "the editor failed; the edits are kept at %s",
			   e->path);
		goto out;
	}
	text = read_text_file(e->path);
	if (!text) {
		fyai_error(ctx, "cannot read %s: %s", e->path, strerror(errno));
		rc = -1;
		goto out;
	}
	rc = fyai_skill_update(ctx, e->name, e->file, text, e->create, -1);
	free(text);
	if (rc) {
		fyai_error(ctx, "the edits are kept at %s", e->path);
		goto out;
	}
	(void)unlink(e->path);
out:
	fyai_cmd_done(call, rc, rc ? fy_invalid :
		      fy_mapping(call->gb, "name", fy_value(call->gb, e->name),
				 "file", fy_value(call->gb,
					e->file ? e->file : "SKILL.md")));
}

static void skill_edit_complete(struct fyai_editor_request *editor,
				void *userdata)
{
	struct skill_edit *e = userdata;
	int rc;

	(void)editor;
	rc = fyai_event_defer(fyai_ctx_loop(e->call->ctx), skill_edit_service,
			      e);
	if (rc) {
		fyai_error(e->call->ctx, "cannot resume the edit of '%s'",
			   e->name);
		fyai_cmd_done(e->call, -1, fy_invalid);
	}
}

/* Open the editor on @text for skill @name; the call ends when it does. */
static int skill_edit_start(struct fyai_cmd_call *call, const char *name,
			    const char *file, const char *text, bool create)
{
	struct fyai_ctx *ctx = call->ctx;
	struct skill_edit *e;
	const char *tmpdir, *ext;
	char tmpl[PATH_MAX];
	size_t len;
	ssize_t wr;
	int fd, rc;

	fyai_error_check(ctx, !call->priv, err, "an edit is in flight");
	e = calloc(1, sizeof(*e));
	fyai_error_check(ctx, e, err, "cannot start the edit of '%s'", name);
	e->call = call;
	e->create = create;
	call->priv = e;
	call->cleanup = skill_edit_cleanup;
	call->cancel = skill_edit_cancel;
	e->name = strdup(name);
	e->file = file ? strdup(file) : NULL;
	fyai_error_check(ctx, e->name && (!file || e->file), err,
			 "cannot start the edit of '%s'", name);
	tmpdir = getenv("TMPDIR");
	if (fy_str_empty(tmpdir))
		tmpdir = "/tmp";
	/* The editor chooses its mode by the extension of the file. */
	ext = file && strrchr(file, '.') && !strchr(strrchr(file, '.'), '/') ?
		strrchr(file, '.') : ".md";
	if (strlen(ext) > 16)
		ext = "";
	rc = snprintf(tmpl, sizeof(tmpl), "%s/fyai-skill-%s-XXXXXX%s", tmpdir,
		      name, ext);
	fyai_error_check(ctx, rc > 0 && rc < (int)sizeof(tmpl), err,
			 "cannot name the file to edit '%s' in", name);
	fd = mkstemps(tmpl, (int)strlen(ext));
	fyai_error_check(ctx, fd >= 0, err, "cannot create %s: %s", tmpl,
			 strerror(errno));
	len = strlen(text);
	wr = write(fd, text, len);
	close(fd);
	fyai_error_check(ctx, wr == (ssize_t)len, err_unlink,
			 "cannot write %s", tmpl);
	e->path = strdup(tmpl);
	fyai_error_check(ctx, e->path, err_unlink,
			 "cannot start the edit of '%s'", name);
	/* An editor in the work pane keeps the UI; one on the terminal takes it. */
	if (!fyai_editor_in_pane(ctx)) {
		rc = fyai_ui_external_begin(ctx);
		fyai_error_check(ctx, !rc, err, "cannot suspend the display; "
				 "the text is at %s", tmpl);
		e->ui_external = true;
	}
	e->editor = fyai_editor_submit(ctx, tmpl, false, skill_edit_complete, e);
	fyai_error_check(ctx, e->editor, err, "the editor did not start; the "
			 "text is at %s", tmpl);
	return FYAI_CMD_PENDING;

err_unlink:
	(void)unlink(tmpl);
err:
	return -1;
}

int fyai_cmd_skill_edit(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *file = fyai_cmd_arg_str(call, "file");
	fy_generic skill, files, text;

	(void)result;
	skill = fyai_skill_get(ctx, name);
	fyai_error_check(ctx, fy_is_valid(skill), err,
			 "no skill '%s' on branch '%s'", name,
			 fyai_ctx_branch(ctx));
	if (file) {
		files = fy_get(skill, "files", fy_map_empty);
		text = fy_get(fy_is_mapping(files) ? files : fy_map_empty, file,
			      fy_invalid);
		/* A file that the skill does not hold yet starts empty. */
		if (!fy_is_string(text))
			text = fy_value(call->gb, "");
	} else {
		text = fyai_skill_source(ctx, call->gb, skill);
		fyai_error_check(ctx, fy_is_string(text), err,
				 "cannot write the SKILL.md of '%s'", name);
	}
	return skill_edit_start(call, name, file, fy_castp(&text, ""), false);
err:
	return -1;
}

int fyai_cmd_skill_new(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	const char *name = fyai_cmd_arg_str(call, "name");

	(void)result;
	fyai_error_check(ctx, fyai_skill_name_valid(name), err,
			 "'%s' is not a usable skill name; use lowercase "
			 "letters, digits and '-', at most %d", name,
			 FYAI_SKILL_NAME_MAX);
	fyai_error_check(ctx, fy_is_invalid(fyai_skill_get(ctx, name)), err,
			 "'%s' is already stored; edit it with skill edit %s",
			 name, name);
	return skill_edit_start(call, name, NULL,
				fyai_skill_template(call->gb, name), true);
err:
	return -1;
}

static int skill_cmd_enable(struct fyai_cmd_call *call, fy_generic *result,
			    bool enabled)
{
	fy_generic names = fy_get(call->args, "names", fy_seq_empty);

	if (fyai_skill_set_enabled(call->ctx, names, enabled))
		return -1;
	*result = fy_mapping(call->gb, "names",
			     fyai_skill_join(call->gb, names, ", "));
	return 0;
}

int fyai_cmd_skill_enable(struct fyai_cmd_call *call, fy_generic *result)
{
	return skill_cmd_enable(call, result, true);
}

int fyai_cmd_skill_disable(struct fyai_cmd_call *call, fy_generic *result)
{
	return skill_cmd_enable(call, result, false);
}

/*
 * `skill exec` runs a skill whose name a command takes. The session reads
 * "/skill exec NAME ARGS" as a user turn before it dispatches a command, as
 * it reads "/NAME ARGS"; this handler runs only when no enabled skill has
 * the name.
 */
int fyai_cmd_skill_exec(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "no enabled skill '%s' on branch '%s'",
		   fyai_cmd_arg_str(call, "name"), fyai_ctx_branch(call->ctx));
	return -1;
}

/* The verb is a prompt run of the line that the session would read. */
int fyai_cmd_skill_exec_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	fy_generic name, rest;
	const char *a;

	name = fy_get(args, "name", fy_invalid);
	rest = fy_get(args, "args", fy_invalid);
	a = fy_is_string(rest) ? fy_castp(&rest, "") : "";
	cfg->prompt = fy_gb_intern_string(cfg->gb, fy_sprintfa(
		"/skill exec %s%s%s", fy_castp(&name, ""), *a ? " " : "", a));
	cfg->interactive = false;
	cfg->cmd.run = FYAI_RUN_PROMPT;
	return 0;
}
