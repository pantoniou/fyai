/*
 * fyai_foreign_import.c - inspect foreign JSONL session snapshots
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_display.h"
#include "fyai_branch.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_foreign_import.h"
#include "fyai_provider.h"
#include "fyai_sink.h"
#include "fyai_storage.h"
#include "fyai_tools.h"
#include "fyai_turn.h"
#include "utils.h"

struct foreign_summary {
	enum fyai_foreign_source source;
	unsigned long records;
	unsigned long messages;
	unsigned long calls;
	unsigned long results;
	unsigned long compactions;
	unsigned long losses;
	long long tokens;
};

struct foreign_build {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *gb;
	fy_generic losses;
	fy_generic calls;
};

struct foreign_display {
	struct fy_generic_builder *gb;
	fy_generic markdown;
	fy_generic fragments;
	bool active;
};

static fy_generic foreign_display_record(struct foreign_display *display,
					 const char *tag)
{
	return fy_mapping(display->gb, "tag", tag,
		"markdown", display->markdown, "state", "finalized",
		"fragments", display->fragments);
}

static fy_generic foreign_display_finish(struct fyai_ctx *ctx,
					 fy_generic head,
					 struct foreign_display *display)
{
	if (!display->active)
		return head;
	head = fyai_turn_append_display_output(ctx, head,
		foreign_display_record(display, "assistant"));
	display->markdown = fy_value(display->gb, "");
	display->fragments = fy_seq_empty;
	display->active = false;
	return head;
}

static fy_generic foreign_display_user(struct fyai_ctx *ctx, fy_generic head,
				       fy_generic text,
				       struct foreign_display *display)
{
	struct foreign_display user = {
		.gb = display->gb,
		.markdown = text,
		.fragments = fy_seq_empty,
		.active = true,
	};

	return fyai_turn_append_display_output(ctx, head,
		foreign_display_record(&user, "user"));
}

static void foreign_display_text(struct foreign_display *display,
				 fy_generic text)
{
	if (!fy_is_string(text) || fy_empty(text))
		return;
	display->markdown = fy_join(display->gb, "", display->markdown,
		text, "\n\n");
	display->active = true;
}

static int foreign_display_call(struct fyai_ctx *ctx,
				struct foreign_display *display,
				fy_generic call)
{
	struct fyai_md_blocks blocks = {0};
	fy_generic type, args, command;
	const char *name, *arguments;
	char *md = NULL;
	size_t mdlen = 0, base, head, i;
	int rc;
	FILE *fp;

	type = fy_get(call, "type", fy_invalid);
	if (fy_equal(type, "shell_call")) {
		name = "shell";
		command = fy_get_at_path(call, "action", "commands", 0);
		args = fy_mapping(display->gb, "command", command);
	} else {
		name = fyai_tool_name_canonical(fy_get(call, "name", "tool"));
		arguments = fy_get(call, "arguments", "{}");
		args = parse_json_string(display->gb, arguments);
	}
	fp = open_memstream(&md, &mdlen);
	fyai_error_check(ctx, fp, err_out, "foreign import: cannot render tool call");
	fyai_emit_tool_call(ctx, fp, display->gb, name, args,
		fyai_tool_preview_lines(ctx->cfg, name), &blocks);
	rc = fclose(fp);
	fp = NULL;
	fyai_error_check(ctx, !rc, err_out,
			 "foreign import: cannot finish tool call rendering");
	base = strlen(fy_castp(&display->markdown, ""));
	head = mdlen;
	for (i = 0; i < mdlen; i++)
		if (md[i] == '\n') {
			head = i;
			break;
		}
	if (head)
		display->fragments = fy_append(display->gb, display->fragments,
			fy_mapping(display->gb, "kind", "tool_head",
				"start", (long long)base,
				"end", (long long)(base + head),
				"tool", name, "ok", true));
	for (i = 0; i < blocks.count; i++)
		display->fragments = fy_append(display->gb, display->fragments,
			fy_mapping(display->gb, "kind", "tool_body",
				"start", (long long)(base + blocks.item[i].start),
				"end", (long long)(base + blocks.item[i].end),
				"lang", blocks.item[i].lang ? : "",
				"tool", name));
	display->markdown = fy_join(display->gb, "", display->markdown, md);
	fyai_error_check(ctx, fy_is_valid(display->markdown), err_out,
			 "foreign import: cannot store tool call display");
	display->active = true;
	fyai_md_blocks_free(&blocks);
	free(md);
	return 0;

err_out:
	if (fp)
		fclose(fp);
	fyai_md_blocks_free(&blocks);
	free(md);
	return -1;
}

static void foreign_display_result(struct foreign_build *build,
				   struct foreign_display *display,
				   fy_generic result)
{
	fy_generic tool;
	const char *id;
	size_t pos;

	id = fy_get(result, "call_id", "");
	tool = *id ? fy_get(build->calls, id) : fy_invalid;
	if (fy_equal(tool, "exec_command"))
		tool = fy_value(display->gb, "shell");
	pos = strlen(fy_castp(&display->markdown, ""));
	display->fragments = fy_append(display->gb, display->fragments,
		fy_mapping(display->gb, "kind", "tool_result",
			"start", (long long)pos, "end", (long long)pos,
			"tool", fy_is_string(tool) ? tool : fy_value(display->gb, "tool")));
	display->active = true;
}

static fy_generic foreign_display_messages(struct fyai_ctx *ctx,
					    struct foreign_build *build,
					    struct foreign_display *display,
					    fy_generic head,
					    fy_generic messages)
{
	fy_generic item, role, type;

	fy_foreach(item, messages) {
		role = fy_get(item, "role", fy_invalid);
		type = fy_get(item, "type", fy_invalid);
		if (fy_equal(role, "user")) {
			head = foreign_display_finish(ctx, head, display);
			head = foreign_display_user(ctx, head,
				fy_get(item, "content", fy_invalid), display);
		} else if (fy_equal(role, "assistant")) {
			foreign_display_text(display,
				fy_get(item, "content", fy_invalid));
		} else if (fyai_item_type_is_call(type)) {
			if (foreign_display_call(ctx, display, item))
				return fy_invalid;
		} else if (fyai_item_type_is_call_output(type)) {
			foreign_display_result(build, display, item);
		}
	}
	return head;
}

static bool foreign_text_type(fy_generic type)
{
	return fy_any_equal(type, "text", "input_text", "output_text");
}

static bool foreign_ignored_content_type(fy_generic type)
{
	return fy_any_equal(type, "thinking", "redacted_thinking");
}

static bool foreign_supported_item_type(fy_generic type)
{
	return fy_any_equal(type, "message", "function_call",
			    "custom_tool_call", "function_call_output",
			    "custom_tool_call_output");
}

static void foreign_summary_content(struct foreign_summary *s,
				    fy_generic content)
{
	fy_generic item, type;

	if (!fy_is_sequence(content))
		return;
	fy_foreach(item, content) {
		type = fy_get(item, "type", fy_invalid);
		if (!foreign_text_type(type) &&
		    !fy_any_equal(type, "tool_use", "tool_result") &&
		    !foreign_ignored_content_type(type))
			s->losses++;
	}
}

struct foreign_list {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *row_gb;
	fy_generic rows;
	enum fyai_foreign_source source;
	const char *cwd;
	fy_generic codex_names;
	bool all;
};

static void foreign_name_component(const char *input, char *output,
				   size_t size);

static fy_generic foreign_branch_name(struct fy_generic_builder *gb,
				      enum fyai_foreign_source source,
				      fy_generic id)
{
	char component[FYAI_BRANCH_NAME_MAX + 1];
	char branch[FYAI_BRANCH_NAME_MAX + 1];

	foreign_name_component(fy_castp(&id, ""), component, sizeof(component));
	if (snprintf(branch, sizeof(branch), "import/%s/%s",
		     fyai_foreign_source_name(source), component) >=
	    (int)sizeof(branch))
		return fy_invalid;
	return fy_value(gb, branch);
}

const char *fyai_foreign_source_name(enum fyai_foreign_source source)
{
	switch (source) {
	case FYAI_FOREIGN_CLAUDE_CODE:
		return "claude-code";
	case FYAI_FOREIGN_CODEX:
		return "codex";
	case FYAI_FOREIGN_AUTO:
		return "auto";
	}
	return "unknown";
}

static enum fyai_foreign_source foreign_record_source(fy_generic record)
{
	fy_generic type, payload;

	type = fy_get(record, "type", fy_invalid);
	if (fy_any_equal(type, "session_meta", "response_item", "event_msg",
			 "turn_context", "compacted"))
		return FYAI_FOREIGN_CODEX;
	payload = fy_get(record, "payload", fy_invalid);
	if (fy_is_valid(payload))
		return FYAI_FOREIGN_CODEX;
	if (fy_is_valid(fy_get(record, "sessionId", fy_invalid)) ||
	    fy_is_valid(fy_get(record, "uuid", fy_invalid)) ||
	    fy_any_equal(type, "assistant", "user", "system"))
		return FYAI_FOREIGN_CLAUDE_CODE;
	return FYAI_FOREIGN_AUTO;
}

static fy_generic foreign_first_text(fy_generic content);
static bool foreign_title_usable(fy_generic title);
static fy_generic foreign_records_read(struct fy_generic_builder *gb,
				       const char *path);

static void foreign_count_claude(struct foreign_summary *s, fy_generic record)
{
	fy_generic message, content, item, type, usage, candidate;
	const char *record_type, *item_type;

	type = fy_get(record, "type", fy_invalid);
	record_type = fy_castp(&type, "");
	message = fy_get(record, "message", fy_invalid);
	content = fy_get(message, "content", fy_seq_empty);
	if (!strcmp(record_type, "user")) {
		candidate = foreign_first_text(content);
		if (fy_is_string(candidate) && !foreign_title_usable(candidate))
			return;
	}
	if (!strcmp(record_type, "assistant") || !strcmp(record_type, "user"))
		s->messages++;
	if (!strcmp(record_type, "system") &&
	    fy_equal(fy_get(record, "subtype", fy_invalid), "compact_boundary"))
		s->compactions++;
	usage = fy_get(message, "usage", fy_invalid);
	s->tokens += fy_get(usage, "input_tokens", 0LL) +
		fy_get(usage, "output_tokens", 0LL);
	foreign_summary_content(s, content);
	fy_foreach(item, content) {
		type = fy_get(item, "type", fy_invalid);
		item_type = fy_castp(&type, "");
		if (!strcmp(item_type, "tool_use"))
			s->calls++;
		else if (!strcmp(item_type, "tool_result"))
			s->results++;
	}
}

static void foreign_count_codex(struct foreign_summary *s, fy_generic record)
{
	fy_generic payload, type, usage;
	const char *record_type, *item_type;

	type = fy_get(record, "type", fy_invalid);
	record_type = fy_castp(&type, "");
	payload = fy_get(record, "payload", fy_invalid);
	if (!strcmp(record_type, "event_msg") &&
	    fy_equal(fy_get(payload, "type", fy_invalid), "token_count")) {
		usage = fy_get(fy_get(payload, "info", fy_invalid),
			       "total_token_usage", fy_invalid);
		s->tokens = fy_get(usage, "total_tokens", s->tokens);
		return;
	}
	if (!strcmp(record_type, "compacted")) {
		s->compactions++;
		return;
	}
	if (strcmp(record_type, "response_item"))
		return;
	type = fy_get(payload, "type", fy_invalid);
	item_type = fy_castp(&type, "");
	if (fy_equal(type, "message")) {
		s->messages++;
		foreign_summary_content(s,
			fy_get(payload, "content", fy_seq_empty));
	}
	else if (fy_any_equal(type, "function_call", "custom_tool_call"))
		s->calls++;
	else if (fy_any_equal(type, "function_call_output",
			      "custom_tool_call_output"))
		s->results++;
	else if (!foreign_supported_item_type(type) &&
		 strcmp(item_type, "reasoning"))
		s->losses++;
}

static int foreign_summary_read(struct fyai_ctx *ctx, const char *path,
				enum fyai_foreign_source requested,
				struct foreign_summary *summary)
{
	fy_generic records, record;
	enum fyai_foreign_source found;

	memset(summary, 0, sizeof(*summary));
	summary->source = requested;
	records = foreign_records_read(ctx->transient_gb,
			path && strcmp(path, "-") ? path : "-");
	fyai_error_check(ctx, fy_is_sequence(records), err_out,
			 "foreign import: cannot parse the JSONL session");
	fy_foreach(record, records) {
		fyai_error_check(ctx, fy_is_mapping(record), err_out,
				 "foreign import: malformed JSONL record %lu",
				 summary->records + 1);
		found = foreign_record_source(record);
		if (found == FYAI_FOREIGN_AUTO)
			continue;
		if (summary->source == FYAI_FOREIGN_AUTO)
			summary->source = found;
		else
			fyai_error_check(ctx, summary->source == found, err_out,
					 "foreign import: record %lu is %s data",
					 summary->records + 1,
					 fyai_foreign_source_name(found));
		summary->records++;
		if (summary->source == FYAI_FOREIGN_CLAUDE_CODE)
			foreign_count_claude(summary, record);
		else
			foreign_count_codex(summary, record);
	}
	fyai_error_check(ctx, summary->source != FYAI_FOREIGN_AUTO &&
			 summary->records, err_out,
			 "foreign import: cannot identify the session format");
	return 0;

err_out:
	return -1;
}

static fy_generic foreign_records_read(struct fy_generic_builder *gb,
				       const char *path)
{
	enum fy_op_parse_flags flags;

	flags = FYOPPF_DISABLE_DIRECTORY | FYOPPF_MULTI_DOCUMENT |
		FYOPPF_MODE_AUTO | FYOPPF_RELAXED_FLOW_DOC;
	return fy_parse_file(gb, flags, path);
}

#define FOREIGN_DISCOVERY_BYTES (64U * 1024U)

/* Discovery reads complete leading records only. Full parsing begins after
 * selection, so picker startup does not scale with transcript byte size. */
static fy_generic foreign_records_peek(struct fy_generic_builder *gb,
				       const char *path)
{
	fy_generic_sized_string input;
	fy_generic records;
	enum fy_op_parse_flags flags;
	struct stat st;
	char *buf;
	ssize_t count, rc;
	size_t size;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return fy_invalid;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		return fy_invalid;
	}
	size = st.st_size < (off_t)FOREIGN_DISCOVERY_BYTES ?
		(size_t)st.st_size : FOREIGN_DISCOVERY_BYTES;
	buf = malloc(size + 1);
	if (!buf) {
		close(fd);
		return fy_invalid;
	}
	count = 0;
	while ((size_t)count < size) {
		rc = read(fd, buf + count, size - (size_t)count);
		if (rc > 0) {
			count += rc;
			continue;
		}
		if (rc < 0 && errno == EINTR)
			continue;
		break;
	}
	close(fd);
	if (count <= 0)
		goto out_invalid;
	if ((off_t)count < st.st_size) {
		while (count > 0 && buf[count - 1] != '\n')
			count--;
		if (!count)
			goto out_invalid;
	}
	buf[count] = '\0';
	flags = FYOPPF_DISABLE_DIRECTORY | FYOPPF_MULTI_DOCUMENT |
		FYOPPF_MODE_AUTO | FYOPPF_RELAXED_FLOW_DOC |
		FYOPPF_INPUT_TYPE_STRING;
	input = (fy_generic_sized_string){ .data = buf, .size = (size_t)count };
	records = fy_parse(gb, input, flags, NULL);
	free(buf);
	return records;

out_invalid:
	free(buf);
	return fy_invalid;
}

static fy_generic foreign_first_text(fy_generic content)
{
	fy_generic item, type;

	if (fy_is_string(content))
		return content;
	if (!fy_is_sequence(content))
		return fy_invalid;
	fy_foreach(item, content) {
		type = fy_get(item, "type", fy_invalid);
		if (fy_any_equal(type, "text", "input_text", "output_text"))
			return fy_get(item, "text", fy_invalid);
	}
	return fy_invalid;
}

static bool foreign_title_usable(fy_generic title)
{
	static const char *const injected[] = {
		"<local-command-caveat>", "<command-name>",
		"<command-message>", "<local-command-stdout>",
	};
	const char *text;
	size_t i;

	if (!fy_is_string(title) || fy_empty(title))
		return false;
	text = fy_castp(&title, "");
	for (i = 0; i < sizeof(injected) / sizeof(*injected); i++)
		if (!strncmp(text, injected[i], strlen(injected[i])))
			return false;
	return true;
}

static fy_generic foreign_title_clean(struct fy_generic_builder *gb,
				      fy_generic title)
{
	const char *text;
	const unsigned char *input;
	char *output;
	fy_generic cleaned;
	size_t i, n;
	bool space;

	if (!fy_is_string(title))
		return fy_invalid;
	text = fy_castp(&title, "");
	input = (const unsigned char *)text;
	output = malloc(strlen(text) + 1);
	if (!output)
		return fy_invalid;
	n = 0;
	space = false;
	for (i = 0; input[i]; i++) {
		if (isspace(input[i])) {
			space = n > 0;
			continue;
		}
		if (space)
			output[n++] = ' ';
		space = false;
		output[n++] = input[i];
	}
	while (n && output[n - 1] == ' ')
		n--;
	output[n] = '\0';
	cleaned = n ? fy_value(gb, output) : fy_invalid;
	free(output);
	return cleaned;
}

static fy_generic foreign_session_row(struct foreign_list *list,
				      const char *path, const struct stat *st)
{
	struct foreign_summary summary = { .source = list->source };
	fy_generic records, record, payload, message, id, cwd, title, fallback;
	fy_generic candidate, type;
	const char *record_type;
	bool codex_context;

	records = foreign_records_peek(list->row_gb, path);
	if (!fy_is_sequence(records))
		return fy_invalid;
	id = fy_invalid;
	cwd = fy_invalid;
	title = fy_invalid;
	fallback = fy_invalid;
	codex_context = false;
	fy_foreach(record, records) {
		summary.records++;
		if (list->source == FYAI_FOREIGN_CLAUDE_CODE)
			foreign_count_claude(&summary, record);
		else
			foreign_count_codex(&summary, record);
		type = fy_get(record, "type", fy_invalid);
		record_type = fy_castp(&type, "");
		if (list->source == FYAI_FOREIGN_CODEX &&
		    !strcmp(record_type, "session_meta")) {
			payload = fy_get(record, "payload", fy_invalid);
			id = fy_get(payload, "session_id",
				    fy_get(payload, "id", fy_invalid));
			cwd = fy_get(payload, "cwd", fy_invalid);
		}
		if (list->source == FYAI_FOREIGN_CODEX &&
		    !strcmp(record_type, "turn_context"))
			codex_context = true;
		if (list->source == FYAI_FOREIGN_CLAUDE_CODE) {
			if (fy_is_invalid(id))
				id = fy_get(record, "sessionId", fy_invalid);
			if (fy_is_invalid(cwd))
				cwd = fy_get(record, "cwd", fy_invalid);
		}
		if (fy_is_invalid(title) && !strcmp(record_type, "user")) {
			message = fy_get(record, "message", fy_invalid);
			candidate = foreign_first_text(fy_get(message, "content",
							      fy_invalid));
			if (foreign_title_usable(candidate))
				title = candidate;
		} else if (!strcmp(record_type, "response_item")) {
			payload = fy_get(record, "payload", fy_invalid);
			if (fy_equal(fy_get(payload, "type", fy_invalid),
				     "message") &&
			    fy_equal(fy_get(payload, "role", fy_invalid), "user")) {
				candidate = foreign_first_text(
					fy_get(payload, "content", fy_invalid));
				if (fy_is_invalid(fallback))
					fallback = candidate;
				if (codex_context && fy_is_invalid(title))
					title = candidate;
			}
		}
	}
	if (fy_is_invalid(title))
		title = fallback;
	if (!fy_is_string(id))
		return fy_invalid;
	if (list->source == FYAI_FOREIGN_CODEX &&
	    fy_is_mapping(list->codex_names)) {
		candidate = fy_get(list->codex_names, fy_castp(&id, ""));
		if (fy_is_string(candidate) && !fy_empty(candidate))
			title = candidate;
	}
	title = foreign_title_clean(list->row_gb, title);
	if (!list->all && list->cwd &&
	    (!fy_is_string(cwd) || strcmp(fy_castp(&cwd, ""), list->cwd)))
		return fy_invalid;
	id = fy_gb_internalize(list->row_gb, id);
	if (fy_is_valid(cwd))
		cwd = fy_gb_internalize(list->row_gb, cwd);
	return fy_mapping(list->row_gb,
			  "source", fyai_foreign_source_name(list->source),
			  "id", id, "cwd", fyai_generic_or_null(cwd),
			  "title", fyai_generic_or_null(title),
			  "turns", (long long)summary.messages +
				(summary.messages ? 1 : 0),
			  "tokens", summary.tokens,
			  "updated", (long long)st->st_mtime * 1000000LL,
			  "path", path, "branch",
			  foreign_branch_name(list->row_gb, list->source, id));
}

static bool foreign_jsonl_path(const char *name)
{
	size_t len = strlen(name);

	return len > 6 && !strcmp(name + len - 6, ".jsonl");
}

static int foreign_scan_dir(struct foreign_list *list, const char *dir,
			    bool exclude_subagents)
{
	struct dirent *de;
	struct stat st;
	DIR *dp;
	char path[PATH_MAX];
	fy_generic row;
	int rc;

	dp = opendir(dir);
	if (!dp)
		return errno == ENOENT ? 0 : -1;
	rc = 0;
	while ((de = readdir(dp))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >=
		    (int)sizeof(path))
			continue;
		if (lstat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (exclude_subagents && !strcmp(de->d_name, "subagents"))
				continue;
			if (foreign_scan_dir(list, path, exclude_subagents))
				rc = -1;
			continue;
		}
		if (!S_ISREG(st.st_mode) || !foreign_jsonl_path(de->d_name))
			continue;
		row = foreign_session_row(list, path, &st);
		if (fy_is_valid(row))
			list->rows = fy_append(list->row_gb,
					       list->rows, row);
	}
	closedir(dp);
	return rc;
}

static char *foreign_default_root(enum fyai_foreign_source source)
{
	const char *value, *home, *suffix;
	size_t len;
	char *path;

	value = getenv(source == FYAI_FOREIGN_CODEX ?
		       "CODEX_HOME" : "CLAUDE_CONFIG_DIR");
	if (value && *value)
		return strdup(value);
	home = getenv("HOME");
	if (!home || !*home)
		return NULL;
	suffix = source == FYAI_FOREIGN_CODEX ? "/.codex" : "/.claude";
	len = strlen(home) + strlen(suffix) + 1;
	path = malloc(len);
	if (path)
		snprintf(path, len, "%s%s", home, suffix);
	return path;
}

static int foreign_scan_source(struct foreign_list *list, const char *root)
{
	char path[PATH_MAX];
	int rc = 0;

	if (list->source == FYAI_FOREIGN_CLAUDE_CODE) {
		if (snprintf(path, sizeof(path), "%s/projects", root) >=
		    (int)sizeof(path))
			return -1;
		return foreign_scan_dir(list, path, true);
	}
	if (snprintf(path, sizeof(path), "%s/sessions", root) >=
	    (int)sizeof(path))
		return -1;
	if (foreign_scan_dir(list, path, false))
		rc = -1;
	if (snprintf(path, sizeof(path), "%s/archived_sessions", root) >=
	    (int)sizeof(path))
		return -1;
	if (foreign_scan_dir(list, path, false))
		rc = -1;
	return rc;
}

static int foreign_list_one(struct foreign_list *list, const char *root_opt)
{
	char *allocated;
	const char *root;
	struct stat st;
	fy_generic records, record, id, name;
	char index[PATH_MAX];
	int rc;

	allocated = NULL;
	root = root_opt;
	if (!root) {
		allocated = foreign_default_root(list->source);
		root = allocated;
	}
	if (!root)
		return 0;
	if (root_opt && (stat(root, &st) || !S_ISDIR(st.st_mode))) {
		free(allocated);
		return -1;
	}
	list->codex_names = fy_invalid;
	if (list->source == FYAI_FOREIGN_CODEX &&
	    snprintf(index, sizeof(index), "%s/session_index.jsonl", root) <
			(int)sizeof(index) && !access(index, R_OK)) {
		records = foreign_records_read(list->row_gb, index);
		if (fy_is_sequence(records)) {
			list->codex_names = fy_map_empty;
			fy_foreach(record, records) {
				id = fy_get(record, "id", fy_invalid);
				name = fy_get(record, "thread_name", fy_invalid);
				if (fy_is_string(id) && fy_is_string(name))
					list->codex_names = fy_assoc(list->row_gb,
						list->codex_names, fy_castp(&id, ""), name);
			}
		}
	}
	rc = foreign_scan_source(list, root);
	free(allocated);
	return rc;
}

int fyai_foreign_import_list(struct fyai_ctx *ctx,
			     enum fyai_foreign_source source,
			     const char *source_root, bool all, bool json)
{
	struct foreign_list list;
	fy_generic row, emitted;
	char cwd[PATH_MAX];
	const char *row_cwd;
	int rc;

	memset(&list, 0, sizeof(list));
	list.ctx = ctx;
	list.row_gb = ctx->transient_gb;
	list.rows = fy_seq_empty;
	list.cwd = getcwd(cwd, sizeof(cwd)) ? cwd : NULL;
	list.all = all;
	rc = 0;
	if (source == FYAI_FOREIGN_AUTO ||
	    source == FYAI_FOREIGN_CLAUDE_CODE) {
		list.source = FYAI_FOREIGN_CLAUDE_CODE;
		if (foreign_list_one(&list, source_root))
			rc = -1;
	}
	if (source == FYAI_FOREIGN_AUTO || source == FYAI_FOREIGN_CODEX) {
		list.source = FYAI_FOREIGN_CODEX;
		if (foreign_list_one(&list, source_root))
			rc = -1;
	}
	if (rc) {
		fyai_error(ctx, "foreign import: cannot scan a session directory");
		return -1;
	}
	if (json) {
		emitted = fy_gb_emit(ctx->transient_gb, list.rows,
				     FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_JSON |
				     FYOPEF_STYLE_COMPACT | FYOPEF_WIDTH_INF,
				     NULL);
		if (!fy_is_string(emitted))
			return -1;
		fyai_result(ctx, "%s\n", fy_castp(&emitted, "[]"));
		return 0;
	}
	fy_foreach(row, list.rows) {
		row_cwd = fy_get(row, "cwd", "");
		fyai_result(ctx, "%s\t%s\t%s\t%s\n",
			    fy_get(row, "source", ""), fy_get(row, "id", ""),
			    row_cwd, fy_get(row, "title", ""));
	}
	return 0;
}

fy_generic fyai_foreign_sessions(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb,
				 enum fyai_foreign_source source,
				 const char *cwd, bool all)
{
	struct foreign_list list;

	memset(&list, 0, sizeof(list));
	list.ctx = ctx;
	list.row_gb = gb;
	list.rows = fy_seq_empty;
	list.cwd = cwd;
	list.all = all;
	if (source == FYAI_FOREIGN_AUTO ||
	    source == FYAI_FOREIGN_CLAUDE_CODE) {
		list.source = FYAI_FOREIGN_CLAUDE_CODE;
		if (foreign_list_one(&list, NULL))
			return fy_invalid;
	}
	if (source == FYAI_FOREIGN_AUTO || source == FYAI_FOREIGN_CODEX) {
		list.source = FYAI_FOREIGN_CODEX;
		if (foreign_list_one(&list, NULL))
			return fy_invalid;
	}
	return list.rows;
}

static fy_generic foreign_loss(struct foreign_build *build, const char *kind,
			       fy_generic type, const char *placeholder)
{
	fy_generic loss;

	loss = fy_mapping(build->gb, "kind", kind, "content_type",
			  fy_is_string(type) ? type : fy_value(build->gb, "unknown"));
	build->losses = fy_append(build->gb, build->losses, loss);
	return fy_value(build->gb, placeholder);
}

static void foreign_mapping_loss(struct foreign_build *build,
				 const char *tool, const char *reason)
{
	build->losses = fy_append(build->gb, build->losses,
		fy_mapping(build->gb, "kind", "tool_mapping", "tool", tool,
			"reason", reason));
}

static fy_generic foreign_content_text(struct foreign_build *build,
				       fy_generic content)
{
	fy_generic item, text, joined, type;

	if (fy_is_string(content))
		return content;
	joined = fy_value(build->gb, "");
	if (!fy_is_sequence(content))
		return joined;
	fy_foreach(item, content) {
		type = fy_get(item, "type", fy_invalid);
		if (foreign_ignored_content_type(type) ||
		    fy_any_equal(type, "tool_use", "tool_result"))
			continue;
		if (foreign_text_type(type)) {
			text = fy_get(item, "text", fy_invalid);
			if (!fy_is_string(text))
				continue;
		} else {
			text = foreign_loss(build, "unsupported_content", type,
					    "[foreign content omitted]");
		}
		joined = fy_join(build->gb, "", joined, text);
	}
	return joined;
}

static fy_generic foreign_arguments(struct fy_generic_builder *gb,
				    fy_generic args)
{
	if (fy_is_string(args))
		return args;
	if (fy_is_invalid(args) || fy_is_null(args))
		args = fy_map_empty;
	return fy_gb_emit(gb, args,
			  FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_JSON |
			  FYOPEF_STYLE_COMPACT | FYOPEF_WIDTH_INF, NULL);
}

static fy_generic foreign_call(struct fy_generic_builder *gb,
			       fy_generic item, unsigned int *sequence)
{
	fy_generic id, args;
	char generated[64];

	id = fy_get(item, "call_id",
		    fy_get(item, "id", fy_invalid));
	if (!fy_is_string(id)) {
		snprintf(generated, sizeof(generated), "foreign_call_%u",
			 ++*sequence);
		id = fy_value(gb, generated);
	}
	args = fy_get(item, "arguments",
		      fy_get(item, "input", fy_invalid));
	args = foreign_arguments(gb, args);
	return fy_mapping(gb, "type", "function_call", "call_id", id,
			  "name", fy_get(item, "name", "foreign_tool"),
			  "arguments", args);
}

static fy_generic foreign_mapped_call(struct foreign_build *build,
				      fy_generic item, unsigned int *sequence,
				      const char *name, fy_generic args)
{
	fy_generic converted, call, id;

	converted = fy_assoc(build->gb, item, "name", name);
	converted = fy_assoc(build->gb, converted, "arguments", args);
	call = foreign_call(build->gb, converted, sequence);
	id = fy_get(call, "call_id", fy_invalid);
	if (fy_is_string(id))
		build->calls = fy_assoc(build->gb, build->calls,
			fy_castp(&id, ""), fy_value(build->gb, name));
	return call;
}

static const char *foreign_json_end(const char *start)
{
	const char *p;
	char open, close;
	unsigned int depth;
	bool quoted, escaped;

	if (!start || !strchr("{[\"", *start))
		return NULL;
	if (*start == '"') {
		open = close = '"';
	} else {
		open = *start;
		close = open == '{' ? '}' : ']';
	}
	depth = 0;
	quoted = false;
	escaped = false;
	for (p = start; *p; p++) {
		if (escaped) {
			escaped = false;
			continue;
		}
		if (quoted && *p == '\\') {
			escaped = true;
			continue;
		}
		if (*p == '"') {
			quoted = !quoted;
			if (open == '"' && !quoted)
				return p + 1;
			continue;
		}
		if (quoted)
			continue;
		if (*p == open)
			depth++;
		else if (*p == close && !--depth)
			return p + 1;
	}
	return NULL;
}

static bool foreign_codex_tool_known(struct foreign_build *build,
				     const char *name)
{
	fy_generic catalog, agents, agent, tools;

	catalog = fyai_catalog_effective(build->ctx->arena_catalog,
					 build->ctx->cfg->gb);
	agents = fy_get(catalog, "agents", fy_invalid);
	fy_foreach(agent, agents) {
		if (!fy_equal(fy_get(agent, "name", fy_invalid), "codex"))
			continue;
		tools = fy_get(agent, "tools", fy_invalid);
		return fy_is_valid(fy_get(tools, name));
	}
	return false;
}

static fy_generic foreign_codex_args_clean(struct foreign_build *build,
					   const char *tool,
					   fy_generic args)
{
	if (!fy_is_mapping(args))
		return fy_invalid;
	if (strcmp(tool, "exec_command"))
		return args;
	args = fy_assoc(build->gb, args, "command", fy_get(args, "cmd", ""));
	args = fy_disassoc(build->gb, args, "cmd");
	args = fy_disassoc(build->gb, args, "yield_time_ms");
	args = fy_disassoc(build->gb, args, "justification");
	args = fy_disassoc(build->gb, args, "prefix_rule");
	return fy_disassoc(build->gb, args, "sandbox_permissions");
}

static fy_generic foreign_codex_tool_args(struct foreign_build *build,
					   const char *input,
					   const char **tool_name)
{
	const char *call, *name, *arg, *end, *assignment;
	size_t name_len;
	fy_generic args, patch;
	char tool[128];

	call = input ? strstr(input, "tools.") : NULL;
	if (!call)
		return fy_invalid;
	name = call + strlen("tools.");
	end = strchr(name, '(');
	if (!end || end == name || (size_t)(end - name) >= sizeof(tool))
		return fy_invalid;
	name_len = (size_t)(end - name);
	memcpy(tool, name, name_len);
	tool[name_len] = '\0';
	if (!foreign_codex_tool_known(build, tool))
		return fy_invalid;
	*tool_name = fy_gb_intern_string(build->gb, tool);
	arg = end + 1;
	while (*arg == ' ' || *arg == '\t' || *arg == '\n')
		arg++;
	end = foreign_json_end(arg);
	if (end)
		args = parse_json_string_size(build->gb, arg, (size_t)(end - arg));
	else
		args = fy_invalid;
	if (!strcmp(tool, "apply_patch") && !fy_is_string(args)) {
		assignment = strstr(input, "const patch = ");
		if (assignment) {
			assignment += strlen("const patch = ");
			end = foreign_json_end(assignment);
			patch = end ? parse_json_string_size(build->gb, assignment,
						(size_t)(end - assignment)) : fy_invalid;
			if (fy_is_string(patch))
				args = fy_mapping(build->gb, "patch", patch);
		}
	}
	return foreign_codex_args_clean(build, tool, args);
}

static fy_generic foreign_codex_call(struct foreign_build *build,
				     fy_generic item,
				     unsigned int *sequence)
{
	fy_generic input, args, converted, call, id, name, arguments;
	const char *tool_name;

	if (!fy_equal(fy_get(item, "type", fy_invalid), "custom_tool_call")) {
		call = foreign_call(build->gb, item, sequence);
		name = fy_get(call, "name", fy_invalid);
		if (fy_equal(name, "exec_command")) {
			arguments = fy_get(call, "arguments", fy_invalid);
			args = parse_json_string(build->gb,
				fy_castp(&arguments, "{}"));
			args = foreign_codex_args_clean(build, "exec_command", args);
			if (fy_is_mapping(args)) {
				call = foreign_mapped_call(build, item, sequence,
					"exec_command", args);
			}
		}
		goto record;
	}
	input = fy_get(item, "input", fy_invalid);
	tool_name = NULL;
	args = foreign_codex_tool_args(build, fy_castp(&input, ""), &tool_name);
	if (!tool_name || !fy_is_mapping(args))
		call = foreign_call(build->gb, item, sequence);
	else {
		converted = fy_assoc(build->gb, item, "name", tool_name);
		converted = fy_assoc(build->gb, converted, "arguments", args);
		call = foreign_call(build->gb, converted, sequence);
	}
record:
	id = fy_get(call, "call_id", fy_invalid);
	name = fy_get(call, "name", fy_invalid);
	if (fy_is_string(id) && fy_is_string(name))
		build->calls = fy_assoc(build->gb, build->calls,
			fy_castp(&id, ""), name);
	if (fy_is_string(name) &&
	    !fy_any_equal(name, "exec_command", "apply_patch", "read_file",
			  "write_file"))
		foreign_mapping_loss(build, fy_castp(&name, "foreign_tool"),
			"no native fyai tool mapping");
	return call;
}

static fy_generic foreign_output(struct foreign_build *build,
				 fy_generic item, unsigned int *sequence)
{
	struct fy_generic_builder *gb = build->gb;
	fy_generic id, output, part, text, joined, tool;
	const char *value, *body;
	char generated[64];

	id = fy_get(item, "call_id",
		    fy_get(item, "tool_use_id", fy_invalid));
	if (!fy_is_string(id)) {
		snprintf(generated, sizeof(generated), "foreign_orphan_%u",
			 ++*sequence);
		id = fy_value(gb, generated);
	}
	output = fy_get(item, "output", fy_invalid);
	if (fy_is_invalid(output))
		output = fy_get(item, "content", fy_invalid);
	if (fy_is_invalid(output) || fy_is_null(output))
		output = fy_value(gb, "result unavailable");
	if (fy_is_sequence(output)) {
		joined = fy_value(gb, "");
		fy_foreach(part, output) {
			text = fy_get(part, "text", fy_invalid);
			if (!fy_is_string(text))
				continue;
			value = fy_castp(&text, "");
			if (!strncmp(value, "Script completed\n", 17)) {
				body = strstr(value, "\nOutput:\n");
				value = body ? body + strlen("\nOutput:\n") : value;
			}
			if (!strcmp(value, "exit=0"))
				continue;
			joined = fy_join(gb, "", joined, value);
		}
		output = joined;
	}
	tool = fy_get(build->calls, fy_castp(&id, ""));
	if (fy_equal(tool, "apply_patch") && fy_equal(output, "{}"))
		output = fy_value(gb, "");
	else if (!fy_is_string(output))
		output = fy_gb_emit(gb, output,
				    FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_JSON |
				    FYOPEF_STYLE_COMPACT | FYOPEF_WIDTH_INF,
				    NULL);
	return fy_mapping(gb, "type", "function_call_output",
			  "call_id", id, "output", output);
}

static fy_generic foreign_codex_item(struct foreign_build *build,
				     fy_generic item,
				     unsigned int *sequence)
{
	fy_generic type, text;
	const char *name;

	type = fy_get(item, "type", fy_invalid);
	name = fy_castp(&type, "");
	if (!strcmp(name, "message")) {
		text = foreign_content_text(build,
					    fy_get(item, "content", fy_invalid));
		return fy_mapping(build->gb, "role", fy_get(item, "role", "user"),
				  "content", text);
	}
	if (!strcmp(name, "function_call") ||
	    !strcmp(name, "custom_tool_call"))
		return foreign_codex_call(build, item, sequence);
	if (!strcmp(name, "function_call_output") ||
	    !strcmp(name, "custom_tool_call_output"))
		return foreign_output(build, item, sequence);
	if (strcmp(name, "reasoning"))
		foreign_loss(build, "unsupported_item", type,
			     "[foreign item omitted]");
	return fy_invalid;
}

static fy_generic foreign_claude_call(struct foreign_build *build,
				      fy_generic item,
				      unsigned int *sequence)
{
	fy_generic input, args;
	const char *name;

	name = fy_get(item, "name", "foreign_tool");
	input = fy_get(item, "input", fy_map_empty);
	if (!strcmp(name, "Write")) {
		args = fy_mapping(build->gb,
			"path", fy_get(input, "file_path", ""),
			"content", fy_get(input, "content", ""));
		return foreign_mapped_call(build, item, sequence,
			"write_file", args);
	}
	if (!strcmp(name, "Read") &&
	    !fy_is_valid(fy_get(input, "pages", fy_invalid))) {
		args = fy_mapping(build->gb,
			"path", fy_get(input, "file_path", ""));
		if (fy_is_valid(fy_get(input, "offset", fy_invalid)))
			args = fy_assoc(build->gb, args, "offset",
				fy_get(input, "offset", fy_invalid));
		if (fy_is_valid(fy_get(input, "limit", fy_invalid)))
			args = fy_assoc(build->gb, args, "limit",
				fy_get(input, "limit", fy_invalid));
		return foreign_mapped_call(build, item, sequence,
			"read_file", args);
	}
	if (!strcmp(name, "Bash") &&
	    !fy_get(input, "run_in_background", false) &&
	    !fy_get(input, "dangerouslyDisableSandbox", false)) {
		args = fy_mapping(build->gb,
			"command", fy_get(input, "command", ""));
		if (fy_is_valid(fy_get(input, "timeout", fy_invalid)))
			args = fy_assoc(build->gb, args, "timeout",
				fy_get(input, "timeout", fy_invalid));
		if (fy_is_valid(fy_get(input, "description", fy_invalid)))
			args = fy_assoc(build->gb, args, "description",
				fy_get(input, "description", fy_invalid));
		return foreign_mapped_call(build, item, sequence,
			"exec_command", args);
	}
	if (!strcmp(name, "Read"))
		foreign_mapping_loss(build, name,
			"page selection has no native read_file equivalent");
	else if (!strcmp(name, "Bash"))
		foreign_mapping_loss(build, name,
			"background or sandbox semantics have no native equivalent");
	else
		foreign_mapping_loss(build, name,
			"no native fyai tool mapping");
	return foreign_mapped_call(build, item, sequence, name, input);
}

static fy_generic foreign_claude_messages(struct foreign_build *build,
					  fy_generic record,
					  unsigned int *sequence)
{
	fy_generic message, content, item, messages, text, type;
	const char *role, *name;

	message = fy_get(record, "message", fy_invalid);
	role = fy_get(message, "role", fy_get(record, "type", "user"));
	content = fy_get(message, "content", fy_invalid);
	messages = fy_seq_empty;
	if (fy_is_string(content))
		return fy_sequence(build->gb,
			fy_mapping(build->gb, "role", role, "content", content));
	text = foreign_content_text(build, content);
	if (fy_is_string(text) && !fy_empty(text))
		messages = fy_append(build->gb, messages,
			fy_mapping(build->gb, "role", role, "content", text));
	fy_foreach(item, content) {
		type = fy_get(item, "type", fy_invalid);
		name = fy_castp(&type, "");
		if (!strcmp(name, "tool_use"))
			messages = fy_append(build->gb, messages,
					     foreign_claude_call(build, item,
							 sequence));
		else if (!strcmp(name, "tool_result"))
			messages = fy_append(build->gb, messages,
					     foreign_output(build, item, sequence));
	}
	return messages;
}

static fy_generic foreign_system_head(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic messages;

	messages = fy_sequence(gb,
		fy_mapping(gb, "role", "system", "content",
			   ctx->cfg->system_prompt ? ctx->cfg->system_prompt : ""));
	return fyai_turn_append(ctx, fy_invalid, messages);
}

static fy_generic foreign_append_turn(struct fyai_ctx *ctx, fy_generic head,
				      fy_generic messages, const char *source)
{
	fy_generic turn, meta;

	if (!fy_is_sequence(messages) || fy_empty(messages))
		return head;
	turn = fyai_turn_append(ctx, head, messages);
	if (!fy_is_valid(turn))
		return fy_invalid;
	meta = fyai_turn_meta(turn);
	meta = fy_assoc(ctx->transient_gb, meta, "imported", true);
	meta = fy_assoc(ctx->transient_gb, meta, "import_source", source);
	return fy_assoc(ctx->transient_gb, turn, "metadata", meta);
}

static void foreign_name_component(const char *input, char *output, size_t size)
{
	size_t i, n;
	unsigned char c;

	n = 0;
	for (i = 0; input && input[i] && n + 1 < size; i++) {
		c = (unsigned char)input[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
			output[n++] = (char)c;
		else
			output[n++] = '-';
	}
	if (!n && size > 1)
		output[n++] = 'x';
	output[n] = '\0';
}

static int foreign_import_build(struct fyai_ctx *ctx, const char *path,
				enum fyai_foreign_source requested,
				const char *title_override, bool preview,
				int max_exchanges, int max_rows)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic records, record, payload, messages, item, head, candidate;
	fy_generic canonical;
	fy_generic id, cwd, title, fallback, provenance, type;
	fy_generic existing_source, existing_id;
	struct fyai_branch existing;
	enum fyai_foreign_source source, found;
	const char *record_type, *source_name, *destination;
	char component[FYAI_BRANCH_NAME_MAX + 1];
	char branch[FYAI_BRANCH_NAME_MAX + 1];
	unsigned int call_sequence, compactions;
	struct foreign_build build;
	struct foreign_display display;
	bool codex_has_context, codex_context;

	records = foreign_records_read(ctx->transient_gb,
				path && strcmp(path, "-") ? path : "-");
	if (!fy_is_sequence(records)) {
		fyai_error(ctx, "foreign import: cannot parse the JSONL session");
		return -1;
	}
	source = requested;
	id = fy_invalid;
	cwd = fy_invalid;
	title = fy_invalid;
	fallback = fy_invalid;
	head = foreign_system_head(ctx);
	call_sequence = 0;
	compactions = 0;
	build.ctx = ctx;
	build.gb = gb;
	build.losses = fy_seq_empty;
	build.calls = fy_map_empty;
	display.gb = gb;
	display.markdown = fy_value(gb, "");
	display.fragments = fy_seq_empty;
	display.active = false;
	codex_has_context = false;
	codex_context = false;
	fy_foreach(record, records)
		if (fy_equal(fy_get(record, "type", fy_invalid), "turn_context"))
			codex_has_context = true;
	fy_foreach(record, records) {
		found = foreign_record_source(record);
		if (found != FYAI_FOREIGN_AUTO) {
			if (source == FYAI_FOREIGN_AUTO)
				source = found;
			else if (source != found) {
				fyai_error(ctx, "foreign import: mixed session formats");
				return -1;
			}
		}
		type = fy_get(record, "type", fy_invalid);
		record_type = fy_castp(&type, "");
		if (source == FYAI_FOREIGN_CODEX) {
			if (!strcmp(record_type, "session_meta")) {
				payload = fy_get(record, "payload", fy_invalid);
				id = fy_get(payload, "session_id",
					    fy_get(payload, "id", fy_invalid));
				cwd = fy_get(payload, "cwd", fy_invalid);
				continue;
			}
			if (!strcmp(record_type, "turn_context")) {
				codex_context = true;
				continue;
			}
			if (!strcmp(record_type, "compacted")) {
				payload = fy_get(record, "payload", fy_invalid);
				head = foreign_system_head(ctx);
				build.calls = fy_map_empty;
				display.markdown = fy_value(gb, "");
				display.fragments = fy_seq_empty;
				display.active = false;
				fy_foreach(item, fy_get(payload, "replacement_history",
							fy_seq_empty)) {
					item = foreign_codex_item(&build, item,
							   &call_sequence);
					if (fy_is_valid(item))
						head = foreign_append_turn(ctx, head,
							fy_sequence(gb, item), "codex");
				}
				compactions++;
				continue;
			}
			if (strcmp(record_type, "response_item"))
				continue;
			payload = fy_get(record, "payload", fy_invalid);
			if (codex_has_context && !codex_context)
				continue;
			if (fy_equal(fy_get(payload, "type", fy_invalid), "message") &&
			    fy_equal(fy_get(payload, "role", fy_invalid), "user")) {
				candidate = foreign_first_text(
					fy_get(payload, "content", fy_invalid));
				if (fy_is_invalid(fallback))
					fallback = candidate;
				if (fy_is_invalid(title))
					title = candidate;
			}
			canonical = foreign_codex_item(&build, payload,
						       &call_sequence);
			if (!fy_is_valid(canonical))
				continue;
			if (fy_equal(fy_get(canonical, "role", fy_invalid), "user"))
				head = foreign_display_finish(ctx, head, &display);
			head = foreign_append_turn(ctx, head,
				fy_sequence(gb, canonical), "codex");
			if (fy_equal(fy_get(canonical, "role", fy_invalid), "user"))
				head = foreign_display_user(ctx, head,
					fy_get(canonical, "content", fy_invalid), &display);
			else if (fy_equal(fy_get(canonical, "role", fy_invalid),
					 "assistant"))
				foreign_display_text(&display,
					fy_get(canonical, "content", fy_invalid));
			else if (fyai_item_type_is_call(
					 fy_get(canonical, "type", fy_invalid)))
				fyai_error_check(ctx,
						 !foreign_display_call(ctx, &display,
								       canonical),
						 err_out,
						 "foreign import: cannot build display");
			else if (fyai_item_type_is_call_output(
					 fy_get(canonical, "type", fy_invalid)))
				foreign_display_result(&build, &display, canonical);
			continue;
		}
		if (source != FYAI_FOREIGN_CLAUDE_CODE)
			continue;
		if (fy_is_invalid(id))
			id = fy_get(record, "sessionId", fy_invalid);
		if (fy_is_invalid(cwd))
			cwd = fy_get(record, "cwd", fy_invalid);
		if (!strcmp(record_type, "system") &&
		    fy_equal(fy_get(record, "subtype", fy_invalid),
			     "compact_boundary")) {
			head = foreign_system_head(ctx);
			build.calls = fy_map_empty;
			display.markdown = fy_value(gb, "");
			display.fragments = fy_seq_empty;
			display.active = false;
			compactions++;
			continue;
		}
		if (fy_get(record, "isSidechain", false))
			continue;
		if (!fy_any_equal(type, "user", "assistant"))
			continue;
		if (!strcmp(record_type, "user")) {
			candidate = foreign_first_text(
				fy_get(fy_get(record, "message", fy_invalid),
				       "content", fy_invalid));
			if (fy_is_string(candidate) &&
			    !foreign_title_usable(candidate))
				continue;
			if (fy_is_invalid(title) &&
			    foreign_title_usable(candidate))
				title = candidate;
		}
		messages = foreign_claude_messages(&build, record, &call_sequence);
		head = foreign_append_turn(ctx, head, messages, "claude-code");
		head = foreign_display_messages(ctx, &build, &display, head,
			messages);
	}
	if (source == FYAI_FOREIGN_CODEX ||
	    source == FYAI_FOREIGN_CLAUDE_CODE)
		head = foreign_display_finish(ctx, head, &display);
	if (fy_is_invalid(title))
		title = fallback;
	if (title_override && *title_override)
		title = fy_value(gb, title_override);
	title = foreign_title_clean(gb, title);
	if (source == FYAI_FOREIGN_AUTO || !fy_is_string(id) ||
	    !fy_is_valid(head)) {
		fyai_error(ctx, "foreign import: incomplete session identity or context");
		return -1;
	}
	source_name = fyai_foreign_source_name(source);
	provenance = fy_mapping(gb, "version", 1LL, "source", source_name,
				"session_id", id, "cwd", fyai_generic_or_null(cwd),
				"title", fyai_generic_or_null(title),
				"compactions", (long long)compactions,
				"losses", build.losses);
	if (!fy_is_valid(provenance))
		return -1;
	if (preview) {
		ctx->last_message = head;
		return fyai_display_recap(ctx, max_exchanges, max_rows);
	}
	if (ctx->cfg->branch_explicit)
		destination = fyai_ctx_branch(ctx);
	else {
		foreign_name_component(fy_castp(&id, ""), component,
				       sizeof(component));
		if (snprintf(branch, sizeof(branch), "import/%s/%s",
			     source_name, component) >= (int)sizeof(branch)) {
			fyai_error(ctx, "foreign import: generated branch name is too long");
			return -1;
		}
		destination = branch;
	}
	if (fyai_branch_lookup(ctx->arena_branches, destination, &existing)) {
		existing_source = fy_get(existing.import, "source", fy_invalid);
		existing_id = fy_get(existing.import, "session_id", fy_invalid);
		if (fy_equal(existing_source, source_name) && fy_equal(existing_id, id)) {
			fyai_result(ctx, "foreign session is already branch %s\n",
				    destination);
			return 0;
		}
		fyai_error(ctx, "foreign import: branch '%s' already exists",
			   destination);
		return -1;
	}
	if (fyai_branch_import(ctx, destination, head, provenance, cwd,
			       fy_is_string(title) ? fy_castp(&title, "") : NULL))
		return -1;
	if (!fy_empty(build.losses))
		fyai_warning(ctx, "foreign import: preserved %zu unsupported item%s "
			     "as placeholders", fy_len(build.losses),
			     fy_len(build.losses) == 1 ? "" : "s");
	return 0;

err_out:
	return -1;
}

int fyai_foreign_import_view(struct fyai_ctx *ctx, const char *path,
			     enum fyai_foreign_source source,
			     const char *title)
{
	return foreign_import_build(ctx, path, source, title, false, 0, 0);
}

int fyai_foreign_preview(struct fyai_ctx *ctx, const char *path,
			 enum fyai_foreign_source source, int max_exchanges,
			 int max_rows)
{
	return foreign_import_build(ctx, path, source, NULL, true,
				    max_exchanges, max_rows);
}

int fyai_foreign_import_session(struct fyai_ctx *ctx,
				enum fyai_foreign_source source,
				const char *source_root, const char *session,
				bool dry_run, bool json)
{
	struct foreign_list list;
	fy_generic row, id, path, selected;
	char cwd[PATH_MAX];
	int matches;

	if (source == FYAI_FOREIGN_AUTO || !session || !*session)
		return -1;
	memset(&list, 0, sizeof(list));
	list.ctx = ctx;
	list.row_gb = ctx->transient_gb;
	list.rows = fy_seq_empty;
	list.source = source;
	list.cwd = getcwd(cwd, sizeof(cwd)) ? cwd : NULL;
	list.all = true;
	if (foreign_list_one(&list, source_root)) {
		fyai_error(ctx, "foreign import: cannot scan a session directory");
		return -1;
	}
	selected = fy_invalid;
	matches = 0;
	fy_foreach(row, list.rows) {
		id = fy_get(row, "id", fy_invalid);
		if (!fy_equal(id, session))
			continue;
		selected = row;
		matches++;
	}
	if (!matches) {
		fyai_error(ctx, "foreign import: %s session '%s' was not found",
			   fyai_foreign_source_name(source), session);
		return -1;
	}
	if (matches > 1) {
		fyai_error(ctx, "foreign import: %s session ID '%s' is ambiguous",
			   fyai_foreign_source_name(source), session);
		return -1;
	}
	path = fy_get(selected, "path", fy_invalid);
	if (!fy_is_string(path))
		return -1;
	if (dry_run)
		return fyai_foreign_import_dry_run(ctx, fy_castp(&path, ""),
						   source, json);
	return fyai_foreign_import_view(ctx, fy_castp(&path, ""), source,
		fy_get(row, "title", ""));
}

int fyai_foreign_import_dry_run(struct fyai_ctx *ctx, const char *path,
				enum fyai_foreign_source source, bool json)
{
	struct foreign_summary summary;
	fy_generic report, emitted;
	int rc;

	rc = foreign_summary_read(ctx, path, source, &summary);
	if (rc)
		return rc;
	if (json) {
		report = fy_mapping(ctx->transient_gb,
			"source", fyai_foreign_source_name(summary.source),
			"records", (long long)summary.records,
			"messages", (long long)summary.messages,
			"tool_calls", (long long)summary.calls,
			"tool_results", (long long)summary.results,
			"compactions", (long long)summary.compactions,
			"losses", (long long)summary.losses);
		emitted = fy_gb_emit(ctx->transient_gb, report,
			FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_JSON |
			FYOPEF_STYLE_COMPACT | FYOPEF_WIDTH_INF, NULL);
		if (!fy_is_string(emitted))
			return -1;
		fyai_result(ctx, "%s\n", fy_castp(&emitted, "{}"));
	} else
		fyai_result(ctx,
			"foreign session: %s; %lu records, %lu messages, %lu tool "
			"calls, %lu tool results, %lu compactions, %lu losses\n",
			fyai_foreign_source_name(summary.source), summary.records,
			summary.messages, summary.calls, summary.results,
			summary.compactions, summary.losses);
	return 0;
}
