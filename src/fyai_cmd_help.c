/*
 * fyai_cmd_help.c - help and usage from the command definitions
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_sink.h"
#include "commands.h"
#include "fyai_session.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

static const char *surface_prefix(enum fyai_cmd_surface s)
{
	return s == FYAI_CMD_SESSION ? "/" : "fyai ";
}

/* Append @s; a failed append is kept in @failp and reported one time. */
static void put(struct response_buffer *out, bool *failp, const char *s)
{
	if (!*failp && response_buffer_append(out, s))
		*failp = true;
}

static void putf(struct response_buffer *out, bool *failp, const char *fmt,
		 ...) __attribute__((format(printf, 3, 4)));

static void putf(struct response_buffer *out, bool *failp, const char *fmt,
		 ...)
{
	va_list ap;
	char *s;
	int n;

	va_start(ap, fmt);
	n = vasprintf(&s, fmt, ap);
	va_end(ap);
	if (n < 0) {
		*failp = true;
		return;
	}
	put(out, failp, s);
	free(s);
}

/* A table cell holds no line break and no bar. */
static void put_cell(struct response_buffer *out, bool *failp, const char *s)
{
	char c[2] = { 0, 0 };

	for (; s && *s; s++) {
		if (*s == '\n') {
			put(out, failp, " ");
			continue;
		}
		if (*s == '<') {
			put(out, failp, "&lt;");
			continue;
		}
		if (*s == '>') {
			put(out, failp, "&gt;");
			continue;
		}
		if (*s == '|')
			put(out, failp, "\\");
		c[0] = *s;
		put(out, failp, c);
	}
}

static bool word_is_flag(const char *w)
{
	return w[0] == '-';
}

static const char *gstr(fy_generic *v)
{
	return fy_is_string(*v) ? fy_castp(v, "") : "";
}

/* The name of @def and its aliases, each as code: `/reset`, `/rewind`. */
static void put_names(struct response_buffer *out, bool *failp,
		      fy_generic def, const char *pfx)
{
	fy_generic name, alias;

	name = fy_get(def, "command", fy_invalid);
	putf(out, failp, "`%s%s`", pfx, gstr(&name));
	fy_foreach(alias, fy_get(def, "aliases", fy_invalid))
		putf(out, failp, ", `%s%s`", pfx, gstr(&alias));
}

/* ---- usage ---------------------------------------------------------------- */

static void usage_prop(struct response_buffer *out, bool *failp,
		       struct fyai_cmd_prop *cp, bool required)
{
	const char *open = required ? "" : "[", *close = required ? "" : "]";

	if (cp->pos >= 0) {
		putf(out, failp, " %s%s%s%s", open, cp->meta,
		     cp->array || cp->rest ? "..." : "", close);
		return;
	}
	if (cp->sname)
		putf(out, failp, " %s-%c", open, cp->sname);
	else
		putf(out, failp, " %s--%s", open, cp->lname);
	if (!cp->boolean)
		putf(out, failp, " %s", cp->meta);
	put(out, failp, close);
}

static bool prop_required(fy_generic def, const char *name)
{
	return fyai_cmd_seq_has(fy_get(fy_get(def, "arguments", fy_invalid),
				       "required", fy_invalid), name);
}

char *fyai_cmd_usage(fy_generic def, const char *path,
		     enum fyai_cmd_surface surface)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS];
	struct response_buffer out = { 0 };
	fy_generic subs, sub, name, *sdefs;
	bool fail = false, first;
	long long pos;
	size_t nsub, si;
	int n, i;

	putf(&out, &fail, "%s%s", surface_prefix(surface), path);
	subs = fy_get(def, "commands", fy_invalid);
	if (fy_is_valid(subs)) {
		n = fyai_cmd_props(def, surface, props, ARRAY_SIZE(props));
		for (i = 0; i < n; i++)
			if (props[i].pos >= 0)
				usage_prop(&out, &fail, &props[i],
					   prop_required(def, props[i].name));
		put(&out, &fail, " {");
		first = true;
		sdefs = fyai_cmd_defs_sorted(subs, "command", &nsub);
		if (!sdefs)
			fail = true;
		for (si = 0; si < nsub; si++) {
			sub = sdefs[si];
			if (!fyai_cmd_def_on(sub, fy_invalid, surface) ||
			    fy_get(sub, "hidden", false))
				continue;
			name = fy_get(sub, "command", fy_invalid);
			putf(&out, &fail, "%s%s", first ? "" : "|",
			     gstr(&name));
			first = false;
		}
		free(sdefs);
		put(&out, &fail, "} ...");
		goto out;
	}
	n = fyai_cmd_props(def, surface, props, ARRAY_SIZE(props));
	for (i = 0; i < n; i++)
		if (props[i].pos < 0 && !props[i].hidden)
			usage_prop(&out, &fail, &props[i],
				   prop_required(def, props[i].name));
	for (pos = 0; ; pos++) {
		for (i = 0; i < n; i++)
			if (props[i].pos == pos)
				break;
		if (i == n)
			break;
		usage_prop(&out, &fail, &props[i],
			   prop_required(def, props[i].name));
	}
out:
	if (fail) {
		free(out.data);
		return NULL;
	}
	return out.data;
}

/* ---- help of one command ---------------------------------------------------- */

static void help_prop_row(struct response_buffer *out, bool *failp,
			  struct fyai_cmd_prop *cp)
{
	fy_generic d, dflt, en, v;
	bool first;

	put(out, failp, "| `");
	if (cp->pos >= 0)
		put(out, failp, cp->meta);
	else if (cp->sname)
		putf(out, failp, "-%c`, `--%s", cp->sname, cp->lname);
	else
		putf(out, failp, "--%s", cp->lname);
	if (cp->pos < 0 && !cp->boolean)
		putf(out, failp, " %s", cp->meta);
	put(out, failp, "` | ");
	d = fy_get(cp->p, "description", fy_invalid);
	put_cell(out, failp, gstr(&d));
	en = fy_get(cp->p, "enum", fy_invalid);
	if (fy_is_sequence(en)) {
		put(out, failp, " (");
		first = true;
		fy_foreach(v, en) {
			put(out, failp, first ? "" : ", ");
			put_cell(out, failp, fy_is_string(v) ? gstr(&v) :
				 fy_str(v));
			first = false;
		}
		put(out, failp, ")");
	}
	dflt = fy_get(cp->p, "default", fy_invalid);
	if (fy_is_valid(dflt)) {
		put(out, failp, "; default ");
		put_cell(out, failp, fy_is_string(dflt) ? gstr(&dflt) :
			 fy_str(dflt));
	}
	put(out, failp, " |\n");
}

static void help_command(struct response_buffer *out, bool *failp,
			 fy_generic def, const char *path,
			 enum fyai_cmd_surface surface)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS];
	fy_generic title, desc, subs, sub, alias, ex, v, *sdefs;
	size_t nsub, si;
	const char *pfx = surface_prefix(surface);
	char *usage;
	bool first, any;
	int n, i;

	title = fy_get(def, "title", fy_invalid);
	desc = fy_get(def, "description", fy_invalid);
	putf(out, failp, "## %s%s\n\n%s\n\n", pfx, path, gstr(&title));
	usage = fyai_cmd_usage(def, path, surface);
	if (!usage) {
		*failp = true;
		return;
	}
	putf(out, failp, "**Usage:** `%s`\n\n", usage);
	free(usage);
	first = true;
	fy_foreach(alias, fy_get(def, "aliases", fy_invalid)) {
		put(out, failp, first ? "**Aliases:** " : ", ");
		putf(out, failp, "`%s`", gstr(&alias));
		first = false;
	}
	if (!first)
		put(out, failp, "\n\n");
	putf(out, failp, "%s\n", gstr(&desc));

	subs = fy_get(def, "commands", fy_invalid);
	if (fy_is_valid(subs)) {
		put(out, failp, "\n### Commands\n\n| Command | Description |\n"
		    "| --- | --- |\n");
		sdefs = fyai_cmd_defs_sorted(subs, "command", &nsub);
		if (!sdefs)
			*failp = true;
		for (si = 0; si < nsub; si++) {
			sub = sdefs[si];
			if (!fyai_cmd_def_on(sub, fy_invalid, surface) ||
			    fy_get(sub, "hidden", false))
				continue;
			title = fy_get(sub, "title", fy_invalid);
			put(out, failp, "| ");
			put_names(out, failp, sub, "");
			put(out, failp, " | ");
			put_cell(out, failp, gstr(&title));
			put(out, failp, " |\n");
		}
		free(sdefs);
		n = fyai_cmd_props(def, surface, props, ARRAY_SIZE(props));
		any = false;
		for (i = 0; i < n; i++) {
			if (props[i].pos < 0 || props[i].hidden)
				continue;
			if (!any)
				put(out, failp, "\n### Arguments\n\n"
				    "| Argument | Description |\n"
				    "| --- | --- |\n");
			any = true;
			help_prop_row(out, failp, &props[i]);
		}
		v = fy_get(def, "default", fy_invalid);
		if (fy_is_mapping(v))
			v = fy_get(v, fyai_cmd_surface_name(surface),
				   fy_invalid);
		if (fy_is_string(v))
			putf(out, failp, "\nWith no command, `%s` runs.\n",
			     gstr(&v));
	} else {
		n = fyai_cmd_props(def, surface, props, ARRAY_SIZE(props));
		any = false;
		for (i = 0; i < n; i++) {
			if (props[i].pos < 0 || props[i].hidden)
				continue;
			if (!any)
				put(out, failp, "\n### Arguments\n\n"
				    "| Argument | Description |\n"
				    "| --- | --- |\n");
			any = true;
			help_prop_row(out, failp, &props[i]);
		}
		put(out, failp, "\n### Options\n\n| Option | Description |\n"
		    "| --- | --- |\n");
		for (i = 0; i < n; i++)
			if (props[i].pos < 0 && !props[i].hidden)
				help_prop_row(out, failp, &props[i]);
		if (surface == FYAI_CMD_CLI)
			put(out, failp, "| `--output FORMAT` | write the "
			    "result as markdown, json, or yaml; see "
			    "`help output` |\n");
		put(out, failp, "| `-h`, `--help` | show this help |\n");
	}

	first = true;
	fy_foreach(ex, fy_get(def, "examples", fy_invalid)) {
		if (first)
			put(out, failp, "\n### Examples\n\n");
		first = false;
		v = fy_get(ex, "args", fy_invalid);
		putf(out, failp, "    %s%s\n\n", pfx, gstr(&v));
		v = fy_get(ex, "text", fy_invalid);
		putf(out, failp, "%s\n\n", gstr(&v));
	}
	first = true;
	fy_foreach(v, fy_get(def, "see_also", fy_invalid)) {
		put(out, failp, first ? "\n**See also:** " : ", ");
		putf(out, failp, "`%shelp %s`", pfx, gstr(&v));
		first = false;
	}
	if (!first)
		put(out, failp, "\n");
}

/* ---- the command list ------------------------------------------------------- */

static void help_list(struct response_buffer *out, bool *failp,
		      enum fyai_cmd_surface surface)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS];
	fy_generic reg, def, name, title, topic, *defs;
	const char *pfx = surface_prefix(surface);
	size_t nd, d;
	int n, k;

	reg = fyai_cmd_registry();
	if (surface == FYAI_CMD_CLI)
		put(out, failp, "## fyai\n\n"
		    "**Usage:** `fyai [OPTIONS] COMMAND [ARGS]`, "
		    "`fyai [OPTIONS] PROMPT...`\n\n"
		    "With no command, the arguments are the prompt. With no "
		    "arguments, fyai starts an interactive session.\n\n");
	put(out, failp, "### Commands\n\n| Command | Description |\n"
	    "| --- | --- |\n");
	defs = fyai_cmd_defs_sorted(fy_get(reg, "commands", fy_invalid),
				    "command", &nd);
	if (!defs)
		*failp = true;
	for (d = 0; d < nd; d++) {
		def = defs[d];
		if (!fyai_cmd_def_on(def, fy_invalid, surface) ||
		    fy_get(def, "hidden", false))
			continue;
		name = fy_get(def, "command", fy_invalid);
		title = fy_get(def, "title", fy_invalid);
		put(out, failp, "| ");
		put_names(out, failp, def, pfx);
		put(out, failp, " | ");
		put_cell(out, failp, gstr(&title));
		put(out, failp, " |\n");
	}
	free(defs);
	if (surface == FYAI_CMD_CLI) {
		def = fy_get(reg, "global", fy_invalid);
		n = fyai_cmd_props(def, surface, props, ARRAY_SIZE(props));
		put(out, failp, "\n### Global options\n\n"
		    "| Option | Description |\n| --- | --- |\n");
		for (k = 0; k < n; k++)
			help_prop_row(out, failp, &props[k]);
	}
	if (surface == FYAI_CMD_SESSION)
		put(out, failp, "\nA line that starts with `//` goes to the "
		    "model with one slash.\n");
	put(out, failp, "\n### Topics\n\n| Topic | Description |\n"
	    "| --- | --- |\n");
	defs = fyai_cmd_defs_sorted(fy_get(reg, "topics", fy_invalid),
				    "topic", &nd);
	if (!defs)
		*failp = true;
	for (d = 0; d < nd; d++) {
		topic = defs[d];
		name = fy_get(topic, "topic", fy_invalid);
		title = fy_get(topic, "title", fy_invalid);
		putf(out, failp, "| `%shelp %s` | ", pfx, gstr(&name));
		put_cell(out, failp, gstr(&title));
		put(out, failp, " |\n");
	}
	free(defs);
}

static fy_generic help_topic(const char *word)
{
	fy_generic topic;

	fy_foreach(topic, fy_get(fyai_cmd_registry(), "topics", fy_invalid))
		if (fy_equal(fy_get(topic, "topic", fy_invalid), word))
			return topic;
	return fy_invalid;
}

int fyai_cmd_help_source(struct fyai_cfg *cfg, enum fyai_cmd_surface surface,
			 size_t nwords, const char *const *words,
			 struct response_buffer *out)
{
	struct fyai_cmd_walk w;
	fy_generic topic, v;
	bool fail = false;
	size_t i;

	if (!fy_is_valid(fyai_cmd_registry())) {
		fyai_cfg_error(cfg, "help: %s", fyai_cmd_registry_why());
		return -1;
	}
	if (!nwords) {
		help_list(out, &fail, surface);
		goto out;
	}
	if (!fyai_cmd_walk(surface, nwords, words, nwords, &w)) {
		for (i = w.consumed; i < nwords; i++)
			if (!word_is_flag(words[i]))
				break;
		if (i == nwords && fy_is_valid(w.group) &&
		    w.consumed == nwords) {
			/* Help names the group, not the default it runs. */
			*strrchr(w.path, ' ') = '\0';
			help_command(out, &fail, w.group, w.path, surface);
			goto out;
		}
		if (i == nwords) {
			help_command(out, &fail, w.def, w.path, surface);
			goto out;
		}
	}
	if (nwords == 1) {
		topic = help_topic(words[0]);
		if (fy_is_valid(topic)) {
			v = fy_get(topic, "title", fy_invalid);
			putf(out, &fail, "## %s\n\n", gstr(&v));
			v = fy_get(topic, "description", fy_invalid);
			putf(out, &fail, "%s", gstr(&v));
			goto out;
		}
	}
	for (i = 0; i < nwords; i++)
		putf(out, &fail, "%s%s", i ? " " : "", words[i]);
	fyai_cfg_error(cfg, "help: no command or topic '%s'",
		       fail ? words[0] : out->data);
	return -1;
out:
	if (fail) {
		fyai_cfg_error(cfg, "help: cannot build the help text");
		return -1;
	}
	return 0;
}

/* ---- the generated reference -------------------------------------------- */

static void reference_command(struct response_buffer *out, bool *failp,
			      fy_generic def, fy_generic inherited,
			      const char *parent, enum fyai_cmd_surface surface)
{
	fy_generic sub, name, surfaces;
	const char *path;

	if (!fyai_cmd_def_on(def, inherited, surface) ||
	    fy_get(def, "hidden", false))
		return;
	name = fy_get(def, "command", fy_invalid);
	path = parent ? fy_sprintfa("%s %s", parent, gstr(&name)) :
	       fy_sprintfa("%s", gstr(&name));
	surfaces = fyai_cmd_def_surfaces(def, inherited);
	help_command(out, failp, def, path, surface);
	put(out, failp, "\n");
	fy_foreach(sub, fy_get(def, "commands", fy_invalid))
		reference_command(out, failp, sub, surfaces, path, surface);
}

/* Every command of the registry and every topic, as one Markdown document. */
int fyai_cmd_reference_markdown(struct response_buffer *out)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS];
	fy_generic reg, def, topic, v;
	bool fail = false;
	int n, k;

	reg = fyai_cmd_registry();
	if (!fy_is_valid(reg))
		return -1;
	put(out, &fail, "# fyai command reference\n\n"
	    "Generated from `data/commands.yaml` by `fyai help --markdown`.\n"
	    "Do not edit; run `ninja docs-commands` to write it again.\n\n"
	    "## Global options\n\n| Option | Description |\n| --- | --- |\n");
	n = fyai_cmd_props(fy_get(reg, "global", fy_invalid), FYAI_CMD_CLI,
			   props, ARRAY_SIZE(props));
	for (k = 0; k < n; k++)
		help_prop_row(out, &fail, &props[k]);
	put(out, &fail, "\n# Verbs\n\n");
	fy_foreach(def, fy_get(reg, "commands", fy_invalid))
		reference_command(out, &fail, def, fy_invalid, NULL,
				  FYAI_CMD_CLI);
	put(out, &fail, "# Slash commands\n\n");
	fy_foreach(def, fy_get(reg, "commands", fy_invalid))
		reference_command(out, &fail, def, fy_invalid, NULL,
				  FYAI_CMD_SESSION);
	put(out, &fail, "# Topics\n");
	/* A description ends in a newline; a heading opens the next block. */
	fy_foreach(topic, fy_get(reg, "topics", fy_invalid)) {
		v = fy_get(topic, "topic", fy_invalid);
		putf(out, &fail, "\n## %s\n\n", gstr(&v));
		v = fy_get(topic, "description", fy_invalid);
		put(out, &fail, gstr(&v));
	}
	return fail ? -1 : 0;
}

/* Text for roff: no Markdown code marks, and escapes for the roff specials. */
static void roff_text(struct response_buffer *out, bool *failp, const char *s)
{
	bool bol = true;
	char c[2] = { 0, 0 };

	for (; s && *s; s++) {
		if (*s == '`')
			continue;
		if (bol && (*s == '.' || *s == '\''))
			put(out, failp, "\\&");
		if (*s == '\\')
			put(out, failp, "\\e");
		else if (*s == '-')
			put(out, failp, "\\-");
		else {
			c[0] = *s;
			put(out, failp, c);
		}
		bol = *s == '\n';
	}
	if (!bol)
		put(out, failp, "\n");
}

static void roff_props(struct response_buffer *out, bool *failp,
		       fy_generic def, enum fyai_cmd_surface surface)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS];
	fy_generic d;
	int n, k;

	n = fyai_cmd_props(def, surface, props, ARRAY_SIZE(props));
	for (k = 0; k < n; k++) {
		if (props[k].hidden)
			continue;
		put(out, failp, ".TP\n");
		if (props[k].pos >= 0)
			putf(out, failp, "\\fI%s\\fR\n", props[k].meta);
		else if (props[k].sname)
			putf(out, failp, "\\fB\\-%c\\fR, \\fB\\-\\-%s\\fR%s%s\n",
			     props[k].sname, props[k].lname,
			     props[k].boolean ? "" : " ",
			     props[k].boolean ? "" : props[k].meta);
		else
			putf(out, failp, "\\fB\\-\\-%s\\fR%s%s\n",
			     props[k].lname, props[k].boolean ? "" : " ",
			     props[k].boolean ? "" : props[k].meta);
		d = fy_get(props[k].p, "description", fy_invalid);
		roff_text(out, failp, gstr(&d));
	}
}

static void roff_command(struct response_buffer *out, bool *failp,
			 fy_generic def, fy_generic inherited,
			 const char *parent)
{
	fy_generic sub, name, v, surfaces;
	const char *path;
	char *usage;

	if (!fyai_cmd_def_on(def, inherited, FYAI_CMD_CLI) ||
	    fy_get(def, "hidden", false))
		return;
	name = fy_get(def, "command", fy_invalid);
	path = parent ? fy_sprintfa("%s %s", parent, gstr(&name)) :
	       fy_sprintfa("%s", gstr(&name));
	surfaces = fyai_cmd_def_surfaces(def, inherited);
	usage = fyai_cmd_usage(def, path, FYAI_CMD_CLI);
	put(out, failp, ".SS \"");
	roff_text(out, failp, usage ? usage : path);
	/* roff_text ended the line; the title closes the quoted heading. */
	out->len--;
	put(out, failp, "\"\n");
	free(usage);
	v = fy_get(def, "description", fy_invalid);
	roff_text(out, failp, gstr(&v));
	roff_props(out, failp, def, FYAI_CMD_CLI);
	fy_foreach(sub, fy_get(def, "commands", fy_invalid))
		roff_command(out, failp, sub, surfaces, path);
}

/* The manual page, fyai(1). */
int fyai_cmd_reference_man(struct response_buffer *out)
{
	fy_generic reg, def, topic, v;
	bool fail = false;

	reg = fyai_cmd_registry();
	if (!fy_is_valid(reg))
		return -1;
	put(out, &fail, ".TH FYAI 1 \"\" \"fyai\" \"User Commands\"\n"
	    ".SH NAME\nfyai \\- stateless AI coding assistant\n"
	    ".SH SYNOPSIS\n.B fyai\n[\\fIOPTIONS\\fR] \\fICOMMAND\\fR "
	    "[\\fIARGS\\fR]\n.br\n.B fyai\n[\\fIOPTIONS\\fR] "
	    "\\fIPROMPT\\fR...\n.SH DESCRIPTION\n");
	v = fy_get(fy_get(reg, "global", fy_invalid), "description",
		   fy_invalid);
	roff_text(out, &fail, gstr(&v));
	put(out, &fail, ".SH OPTIONS\n");
	roff_props(out, &fail, fy_get(reg, "global", fy_invalid),
		   FYAI_CMD_CLI);
	put(out, &fail, ".SH COMMANDS\n");
	fy_foreach(def, fy_get(reg, "commands", fy_invalid))
		roff_command(out, &fail, def, fy_invalid, NULL);
	put(out, &fail, ".SH TOPICS\n");
	fy_foreach(topic, fy_get(reg, "topics", fy_invalid)) {
		v = fy_get(topic, "topic", fy_invalid);
		putf(out, &fail, ".SS %s\n", gstr(&v));
		v = fy_get(topic, "description", fy_invalid);
		roff_text(out, &fail, gstr(&v));
	}
	return fail ? -1 : 0;
}

int fyai_cmd_help(struct fyai_cmd_call *call, fy_generic *result)
{
	struct response_buffer out = { 0 };
	const char *words[16];
	fy_generic w;
	size_t n;
	int rc;

	if (fyai_cmd_arg_bool(call, "markdown") ||
	    fyai_cmd_arg_bool(call, "man")) {
		rc = fyai_cmd_arg_bool(call, "man") ?
		     fyai_cmd_reference_man(&out) :
		     fyai_cmd_reference_markdown(&out);
		fyai_error_check(call->ctx, !rc, err_free,
				 "help: cannot build the reference");
		/* A document for a file, written as it stands. */
		(void)fyai_sink_write(call->ctx->sink, FYAI_SINK_MACHINE,
				      out.data, out.len);
		free(out.data);
		return 0;
	}
	n = 0;
	fy_foreach(w, fy_get(call->args, "topic", fy_invalid)) {
		fyai_error_check(call->ctx, n < ARRAY_SIZE(words), err,
				 "help: too many words");
		words[n++] = fy_gb_intern_string(call->gb, fy_castp(&w, ""));
	}
	rc = fyai_cmd_help_source(call->ctx->cfg, call->surface, n, words,
				  &out);
	if (!rc) {
		*result = fy_value(call->gb, out.data ? out.data : "");
		rc = fy_is_valid(*result) ? 0 : -1;
	}
	free(out.data);
	return rc;
err_free:
	free(out.data);
err:
	return -1;
}
