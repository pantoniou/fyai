/*
 * fyai_config.c - layered configuration loading for fyai
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_terminal.h"
#include "fyai_markdown.h"
#include "fyai_storage.h"
#include "commands.h"
#include "utils.h"

/* Environment variables fyai reads directly, always allowed from --env. */
static const char *const fyai_env_direct[] = {
	"OPENAI_API_KEY", "OPENAI_MODEL", "OPENAI_URL",
	"ANTHROPIC_API_KEY",
};

/*
 * Build the user config path: $XDG_CONFIG_HOME/fyai/config.yaml, falling
 * back to $HOME/.config/fyai/config.yaml. Returns a malloc'd path or NULL.
 */
static char *user_config_path(void)
{
	const char *xdg;
	const char *home;
	char *path;

	xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) {
		if (asprintf(&path, "%s/fyai/config.yaml", xdg) == -1)
			return NULL;
		return path;
	}

	home = getenv("HOME");
	if (!home || !*home)
		return NULL;

	if (asprintf(&path, "%s/.config/fyai/config.yaml", home) == -1)
		return NULL;
	return path;
}

static bool apply_bool(fy_generic root, const char *key, bool cur)
{
	fy_generic v;

	v = fy_get(root, key);

	if (fy_generic_is_invalid(v))
		return cur;
	return fy_cast(v, cur);
}

static int resolve_secret(const char **out, fy_generic v)
{
	const char *name;
	const char *value;

	if (fy_generic_is_invalid(v))
		return -1;

	if (fy_not_equal(fy_get(v, "type"), "env"))
		return -1;

	name = fy_get(v, "value", "");
	if (!*name)
		return -1;

	value = getenv(name);
	if (!value || !*value)
		return -1;

	*out = value;
	return 0;
}

/*
 * Overlay the keys present in @root onto @cfg. Absent keys leave the
 * existing value untouched, so each layer only overrides what it sets.
 *
 * Returns 0 on success, -1 on a malformed value (e.g. a raw secret).
 */
static int apply_config(struct fyai_cfg *cfg, fy_generic root)
{
	const char *mode;
	fy_generic v, sb;

	if (fy_generic_is_invalid(root))
		return 0;

	if (!resolve_secret(&cfg->api_key, fy_get(root, "api_key")))
		cfg->api_key_explicit = true;

	cfg->model = fy_get(root, "model", cfg->model);
	cfg->system_prompt = fy_get(root, "system_prompt", cfg->system_prompt);
	cfg->api_url = fy_get(root, "api_url", cfg->api_url);
	cfg->arena_dir = fy_get(root, "arena_dir", cfg->arena_dir);

	cfg->temperature = fy_get(root, "temperature",
				cfg->temperature);
	cfg->max_tool_iterations = fy_get(root, "max_tool_iterations",
				cfg->max_tool_iterations);
	cfg->max_tokens = fy_get(root, "max_tokens",
				cfg->max_tokens);
	cfg->top_logprobs = fy_get(root, "top_logprobs",
				cfg->top_logprobs);

	cfg->enable_tools = apply_bool(root, "tools",
				cfg->enable_tools);
	cfg->enable_builtin_shell = apply_bool(root, "builtin_shell",
					       cfg->enable_builtin_shell);
	/* sandbox is either a bool (enable with defaults) or a mapping
	 * { enabled, allow, deny, network }. A mapping enables unless it says
	 * otherwise and is retained for the tool path to read grants from. */
	sb = fy_get(root, "sandbox");
	if (fy_generic_is_mapping(sb)) {
		cfg->sandbox = sb;
		cfg->enable_sandbox = apply_bool(sb, "enabled", true);
	} else {
		cfg->enable_sandbox = apply_bool(root, "sandbox",
				cfg->enable_sandbox);
	}
	cfg->logprobs = apply_bool(root, "logprobs", cfg->logprobs);
	cfg->token_extents = apply_bool(root, "token_extents",
					cfg->token_extents);
	cfg->no_obfuscation = apply_bool(root, "no_obfuscation",
					 cfg->no_obfuscation);
	cfg->response_chain = apply_bool(root, "response_chain",
					 cfg->response_chain);

	v = fy_get(root, "logging");
	if (fy_generic_is_mapping(v)) {
		cfg->wire_logging = apply_bool(v, "wire", cfg->wire_logging);
		cfg->stream_logging = apply_bool(v, "stream",
						  cfg->stream_logging);
		cfg->conversation_logging = apply_bool(v, "conversation",
						       cfg->conversation_logging);
	}

	v = fy_get(root, "reasoning");
	if (!fy_generic_is_invalid(v)) {
		cfg->reasoning_effort = fy_get(v, "effort",
					       cfg->reasoning_effort);
		cfg->reasoning_summary = fy_get(v, "summary",
						cfg->reasoning_summary);
	}

	v = fy_get(root, "api");
	if (!fy_generic_is_invalid(v)) {
		mode = fy_cast(v, "");
		if (!strcmp(mode, "chat-completions") || !strcmp(mode, "chat"))
			cfg->api_mode = FYAI_API_CHAT_COMPLETIONS;
		else if (!strcmp(mode, "responses"))
			cfg->api_mode = FYAI_API_RESPONSES;
		else if (!strcmp(mode, "messages"))
			cfg->api_mode = FYAI_API_MESSAGES;
	}

	/* Stylistic options live in the display: group (the only form). */
	v = fy_get(root, "display");
	if (!fy_generic_is_invalid(v)) {
		cfg->markdown_mode = fy_get(v, "markdown_mode",
					    cfg->markdown_mode);
		cfg->color = fy_get(v, "color", cfg->color);
		cfg->theme = fy_get(v, "theme", cfg->theme);
		cfg->code_theme = fy_get(v, "code_theme", cfg->code_theme);
		cfg->markdown_theme = fy_get(v, "markdown_theme",
					     cfg->markdown_theme);
		cfg->markdown_style = fy_get(v, "markdown_style",
					     cfg->markdown_style);
		cfg->tool_preview_lines = fy_get(v, "tool_preview_lines",
						cfg->tool_preview_lines);
		cfg->markdown = apply_bool(v, "markdown", cfg->markdown);
		cfg->stream = apply_bool(v, "stream", cfg->stream);
		cfg->pretty = apply_bool(v, "pretty", cfg->pretty);
		cfg->cache_info = apply_bool(v, "cache_info", cfg->cache_info);
		cfg->stats = apply_bool(v, "stats", cfg->stats);
	}

	return 0;
}

/*
 * Parse a YAML config file into a generic. A missing file yields fy_invalid
 * (not an error); a malformed file is an error.
 */
static int parse_config_file(struct fy_generic_builder *gb, const char *path,
			     fy_generic *out)
{
	fy_generic root;

	*out = fy_invalid;

	if (!path || access(path, R_OK))
		return 0;

	root = fy_parse_file(gb,
				FYOPPF_DISABLE_DIRECTORY | FYOPPF_MODE_YAML_1_2,
				path);
	if (fy_generic_is_invalid(root)) {
		fprintf(stderr, "failed to parse config file: %s\n", path);
		return -1;
	}

	*out = root;
	return 0;
}

/*
 * Walk a config generic and collect, into @acc, the environment-variable
 * names referenced by every `{ type: env, value: NAME }` mapping (the api_key
 * indirections). Returns the (possibly extended) sequence.
 */
static fy_generic collect_env_refs(struct fy_generic_builder *gb,
				   fy_generic v, fy_generic acc)
{
	enum fy_generic_type t;
	const char *name;
	fy_generic item;
	size_t i;
	size_t n;

	t = fy_generic_get_type(v);

	if (t == FYGT_MAPPING) {
		if (fy_equal(fy_get(v, "type"), "env")) {
			name = fy_get(v, "value", "");
			if (*name)
				acc = fy_append(gb, acc, name);
		}
		n = fy_generic_mapping_get_pair_count(v);
		for (i = 0; i < n; i++)
			acc = collect_env_refs(gb,
				fy_generic_mapping_get_at_value(v, i), acc);
	} else if (t == FYGT_SEQUENCE) {
		fy_foreach(item, v)
			acc = collect_env_refs(gb, item, acc);
	}
	return acc;
}

static bool env_name_allowed(fy_generic names, const char *key)
{
	fy_generic name;
	size_t klen;

	/*
	 * Any provider api-key variable is allowed: the provider is only known
	 * after config load (the model drives it), so we cannot enumerate them
	 * up front. The "_API_KEY" suffix is specific enough for a .env file.
	 */
	klen = strlen(key);
	if (klen > 8 && !strcmp(key + klen - 8, "_API_KEY"))
		return true;

	fy_foreach(name, names)
		if (!strcmp(fy_castp(&name, ""), key))
			return true;
	return false;
}

static bool valid_env_key(const char *k)
{
	if (!isalpha((unsigned char)*k) && *k != '_')
		return false;
	for (k++; *k; k++)
		if (!isalnum((unsigned char)*k) && *k != '_')
			return false;
	return true;
}

/* Reject `${VAR}` and `$VAR` -- we do not perform variable substitution. */
static bool has_substitution(const char *s)
{
	for (; *s; s++)
		if (*s == '$' &&
		    (s[1] == '{' || s[1] == '_' || isalpha((unsigned char)s[1])))
			return true;
	return false;
}

static char *strip(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

/*
 * Apply one line of a --env file. Accepts blank/comment lines, `KEY=VALUE`
 * and `export KEY=VALUE`, with the value optionally wrapped in single or
 * double quotes. Variable substitution is rejected. Only names present in
 * @names (the in-use config-export set) are exported; others are silently
 * ignored.
 */
static int env_apply_line(char *line, fy_generic names, const char *path,
			  int lineno)
{
	char *p;
	char *eq;
	char *key;
	char *val;
	size_t vl;

	p = strip(line);

	if (!*p || *p == '#')
		return 0;

	if (!strncmp(p, "export", 6) && isspace((unsigned char)p[6]))
		p = strip(p + 6);

	eq = strchr(p, '=');
	if (!eq) {
		fprintf(stderr, "%s:%d: expected KEY=VALUE\n", path, lineno);
		return -1;
	}
	*eq = '\0';
	key = strip(p);
	val = strip(eq + 1);

	if (!valid_env_key(key)) {
		fprintf(stderr, "%s:%d: invalid variable name '%s'\n",
			path, lineno, key);
		return -1;
	}

	vl = strlen(val);
	if (vl >= 2 &&
	    ((val[0] == '"' && val[vl - 1] == '"') ||
	     (val[0] == '\'' && val[vl - 1] == '\''))) {
		val[vl - 1] = '\0';
		val++;
	} else if (val[0] == '"' || val[0] == '\'') {
		fprintf(stderr, "%s:%d: unterminated quote\n", path, lineno);
		return -1;
	}

	if (has_substitution(val)) {
		fprintf(stderr,
			"%s:%d: variable substitution is not supported\n",
			path, lineno);
		return -1;
	}

	if (env_name_allowed(names, key))
		setenv(key, val, 1);
	return 0;
}

static int load_env_file(const char *path, fy_generic names)
{
	char *content;
	char *p;
	char *nl;
	size_t len;
	int lineno;
	int rc;

	rc = 0;
	lineno = 0;
	content = read_text_file(path);
	if (!content) {
		fprintf(stderr, "--env: cannot read %s\n", path);
		return -1;
	}

	for (p = content; *p; ) {
		nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		len = strlen(p);
		if (len && p[len - 1] == '\r')
			p[len - 1] = '\0';

		if (env_apply_line(p, names, path, ++lineno)) {
			rc = -1;
			break;
		}
		if (!nl)
			break;
		p = nl + 1;
	}

	free(content);
	return rc;
}

/*
 * Project instruction files (AGENTS.md, then CLAUDE.md), the cross-tool
 * convention for repo-scoped agent guidance. Discovered from the project root
 * down to the cwd so the most specific directory's guidance appears last
 * (closest to the model's recent context), preceded by a global layer under
 * $XDG_CONFIG_HOME/fyai. Each file is fenced with a header naming its path.
 * The combined text is appended to the base system prompt at runtime; the
 * canonical system turn then freezes it for the conversation.
 */
static const char *const fyai_instr_names[] = { "AGENTS.md", "CLAUDE.md" };

/* Append "\n\n# <label>\n\n<file contents>" to *buf (realloc'd) when @path is
 * readable. Silently skips a missing or unreadable file. */
static void instr_append_file(char **buf, size_t *len, const char *path)
{
	char *content, *nb;
	size_t clen, hlen, need;
	int n;

	content = read_text_file(path);
	if (!content)
		return;
	clen = strlen(content);
	/* header + path + two blank lines + content + NUL */
	hlen = strlen("\n\n# ") + strlen(path) + strlen("\n\n");
	need = *len + hlen + clen + 1;
	nb = realloc(*buf, need);
	if (!nb) {
		free(content);
		return;
	}
	*buf = nb;
	n = snprintf(*buf + *len, hlen + clen + 1, "\n\n# %s\n\n%s",
		     path, content);
	if (n > 0)
		*len += (size_t)n;
	free(content);
}

/* Read AGENTS.md then CLAUDE.md in @dir into the accumulator. */
static void instr_append_dir(char **buf, size_t *len, const char *dir)
{
	char path[PATH_MAX];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fyai_instr_names); i++) {
		if (snprintf(path, sizeof(path), "%s/%s", dir,
			     fyai_instr_names[i]) >= (int)sizeof(path))
			continue;
		instr_append_file(buf, len, path);
	}
}

/* True when @dir carries a project marker (.git or .fyai). */
static bool dir_is_project_root(const char *dir)
{
	char probe[PATH_MAX];

	if (snprintf(probe, sizeof(probe), "%s/.git", dir) < (int)sizeof(probe) &&
	    !access(probe, F_OK))
		return true;
	if (snprintf(probe, sizeof(probe), "%s/.fyai", dir) < (int)sizeof(probe) &&
	    !access(probe, F_OK))
		return true;
	return false;
}

/*
 * Nearest project root at or above the cwd (first ancestor carrying a
 * .git/.fyai marker), as a malloc'd absolute path the caller frees, or NULL
 * when none is found before the filesystem root. Used to scope the tool
 * sandbox to the project.
 */
char *fyai_discover_project_root(void)
{
	char cwd[PATH_MAX];
	char *cur, *slash, *out = NULL;

	if (!getcwd(cwd, sizeof(cwd)))
		return NULL;
	cur = strdup(cwd);
	if (!cur)
		return NULL;
	for (;;) {
		if (dir_is_project_root(cur)) {
			out = cur;
			return out;
		}
		slash = strrchr(cur, '/');
		if (!slash || slash == cur)
			break;
		*slash = '\0';
	}
	free(cur);
	return NULL;
}

/*
 * Discover the project instruction text, or NULL when there is none. Ordered
 * outermost-first: the global config dir, then the project root down to the
 * cwd. When no project marker (.git/.fyai) is found between the cwd and the
 * filesystem root, only the cwd is consulted (we never walk the whole tree to
 * / reading stray AGENTS.md files). Returns a malloc'd string; caller frees.
 */
char *fyai_project_instructions(void)
{
	char cwd[PATH_MAX];
	char *ancestors[64];
	char *cur, *slash, *dir, *buf = NULL;
	const char *xdg, *home;
	size_t len = 0, n = 0, i;
	bool found_root = false;

	/* Global layer: $XDG_CONFIG_HOME/fyai (or ~/.config/fyai). */
	xdg = getenv("XDG_CONFIG_HOME");
	home = getenv("HOME");
	if (xdg && *xdg) {
		if (asprintf(&dir, "%s/fyai", xdg) != -1) {
			instr_append_dir(&buf, &len, dir);
			free(dir);
		}
	} else if (home && *home) {
		if (asprintf(&dir, "%s/.config/fyai", home) != -1) {
			instr_append_dir(&buf, &len, dir);
			free(dir);
		}
	}

	if (!getcwd(cwd, sizeof(cwd)))
		return buf;

	/* Collect cwd and its ancestors up to (and including) the project
	 * root; abandon the ancestor set if no root marker is found. */
	cur = strdup(cwd);
	if (!cur)
		return buf;
	for (;;) {
		if (n >= ARRAY_SIZE(ancestors)) {
			found_root = false;
			break;
		}
		ancestors[n] = strdup(cur);
		if (!ancestors[n])
			break;
		n++;
		if (dir_is_project_root(cur)) {
			found_root = true;
			break;
		}
		slash = strrchr(cur, '/');
		if (!slash || slash == cur)	/* reached the fs root */
			break;
		*slash = '\0';
	}
	free(cur);

	/* Outermost-first: the root is the last collected ancestor. */
	if (found_root) {
		for (i = n; i-- > 0; )
			instr_append_dir(&buf, &len, ancestors[i]);
	} else {
		instr_append_dir(&buf, &len, cwd);
	}
	for (i = 0; i < n; i++)
		free(ancestors[i]);

	return buf;
}

int fyai_config_load(struct fyai_cfg *cfg,
		     const char *cli_config, const char *cli_env)
{
	struct fy_generic_builder *gb;
	fy_generic root_explicit;
	fy_generic root_repo;
	fy_generic root_user;
	fy_generic names;
	char *user_path;
	size_t i;
	int rc;

	if (!cfg || !cfg->gb)
		return -1;

	gb = cfg->gb;

	root_explicit = fy_invalid;
	user_path = user_config_path();
	rc = parse_config_file(gb, user_path, &root_user);
	free(user_path);
	if (rc)
		return -1;

	/* The repository config (and catalog) live in the repo arena's
	 * container root. */
	if (fyai_peek_arena_config(NULL, gb, &root_repo, &cfg->catalog))
		return -1;

	/* An explicitly named config file must exist and parse. */
	if (cli_config) {
		if (access(cli_config, R_OK)) {
			fprintf(stderr, "config: cannot read %s\n", cli_config);
			return -1;
		}
		if (parse_config_file(gb, cli_config, &root_explicit))
			return -1;
	}

	/*
	 * Apply a --env file before any secret is resolved, restricted to the
	 * variables fyai uses: the direct OPENAI_* names plus every variable
	 * named by a `type: env` mapping anywhere in the configs.
	 */
	if (cli_env) {
		names = fy_seq_empty;
		for (i = 0; i < sizeof(fyai_env_direct) /
				sizeof(fyai_env_direct[0]); i++)
			names = fy_append(gb, names, fyai_env_direct[i]);
		names = collect_env_refs(gb, root_user, names);
		names = collect_env_refs(gb, root_repo, names);
		names = collect_env_refs(gb, root_explicit, names);

		if (load_env_file(cli_env, names))
			return -1;
	}

	/* Top-level keys: user, then repository, then explicit (most specific). */
	if (apply_config(cfg, root_user))
		return -1;
	if (apply_config(cfg, root_repo))
		return -1;
	if (apply_config(cfg, root_explicit))
		return -1;

	return 0;
}

int fyai_config_show(struct fyai_cfg *cfg)
{
	fy_generic m;

	m = fy_null_filtered_mapping(
		"provider", cfg->provider ? cfg->provider : "",
		"model", cfg->model ? cfg->model : "",
		"api", fyai_api_to_string(cfg->api_mode),
		"api_url", cfg->api_url ? cfg->api_url : "",
		"system_prompt", cfg->system_prompt ? cfg->system_prompt : "",
		"temperature", cfg->temperature,
		"max_tool_iterations", cfg->max_tool_iterations,
		"max_tokens", cfg->max_tokens,
		"sandbox", cfg->enable_sandbox,
		"reasoning_effort", cfg->reasoning_effort && *cfg->reasoning_effort ?
					fy_value(cfg->reasoning_effort) : fy_null,
		"reasoning_summary", cfg->reasoning_summary && *cfg->reasoning_summary ?
					fy_value(cfg->reasoning_summary) : fy_null,
		"logging", fy_mapping(
			"wire", cfg->wire_logging,
			"stream", cfg->stream_logging,
			"conversation", cfg->conversation_logging),
		"display", fy_null_filtered_mapping(
			"markdown", cfg->markdown,
			"stream", cfg->stream,
			"tool_preview_lines", cfg->tool_preview_lines,
			"markdown_mode", cfg->markdown_mode ?
					fy_value(cfg->markdown_mode) : fy_null,
			"color", cfg->color ?
					fy_value(cfg->color) : fy_null,
			"theme", cfg->theme ?
					fy_value(cfg->theme) : fy_null,
			"code_theme", cfg->code_theme ?
					fy_value(cfg->code_theme) : fy_null,
			"markdown_theme", cfg->markdown_theme ?
					fy_value(cfg->markdown_theme) : fy_null,
			"markdown_style", cfg->markdown_style ?
					fy_value(cfg->markdown_style) : fy_null));

	emit_generic_to_stdout(NULL, m, true);
	return 0;
}

int fyai_config_get(struct fyai_ctx *ctx, const char *key)
{
	fy_generic v;

	if (fy_generic_is_invalid(ctx->arena_config)) {
		fprintf(stderr, "config: no config in arena; run fyai init or fyai config import\n");
		return -1;
	}
	v = fy_get_at_pathstr(ctx->gb, ctx->arena_config, key);
	if (fy_generic_is_invalid(v)) {
		fprintf(stderr, "config: key '%s' not set\n", key);
		return -1;
	}
	/* Single-line document: bare scalar, or { a: b } / [ 1, 2 ]. */
	(void)fy_emit(v, FYOPEF_DISABLE_DIRECTORY | FYOPEF_OUTPUT_TYPE_STDOUT |
			 FYOPEF_MODE_YAML_1_2 | FYOPEF_STYLE_ONELINE |
			 FYOPEF_WIDTH_INF, NULL);
	return 0;
}

/* Parse a --set value as a YAML flow document into a typed generic. */
static fy_generic config_parse_value(struct fy_generic_builder *gb,
				     const char *value)
{
	return fy_parse(gb, value,
			FYOPPF_DISABLE_DIRECTORY | FYOPPF_MODE_YAML_1_2 |
			FYOPPF_INPUT_TYPE_STRING, NULL);
}

int fyai_config_set(struct fyai_ctx *ctx, const char *key, const char *value)
{
	struct fy_generic_builder *gb = ctx->gb;
	fy_generic root, v;

	if (!gb) {
		fprintf(stderr, "config: no arena; run fyai init\n");
		return -1;
	}
	v = config_parse_value(gb, value);
	if (fy_generic_is_invalid(v)) {
		fprintf(stderr, "config: cannot parse value '%s'\n", value);
		return -1;
	}
	root = fy_generic_is_valid(ctx->arena_config) ?
	       ctx->arena_config : fy_map_empty;
	root = fy_set_at_pathstr(gb, root, key, v);
	if (fy_generic_is_invalid(root)) {
		fprintf(stderr, "config: set failed\n");
		return -1;
	}
	if (fyai_config_has_raw_secret(root)) {
		fprintf(stderr,
			"config: refusing a raw api_key; use { type: env, value: NAME }\n");
		return -1;
	}
	return fyai_publish_root(ctx, root, fy_invalid, fy_invalid);
}

/*
 * Replay the pending --set/--delete/--get ops against the open arena, in order:
 * sets and deletes mutate (and, unless transient, persist) the arena config;
 * gets print the effective value. Runs once storage is open.
 */
int fyai_apply_config_ops(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_config_op *co;
	size_t i;
	int rc;

	if (!cfg->config_op_count)
		return 0;
	if (!ctx->gb) {
		fprintf(stderr,
			"--set/--get/--delete need an arena; run fyai init\n");
		return -1;
	}
	for (i = 0; i < cfg->config_op_count; i++) {
		co = &cfg->config_ops[i];
		switch (co->op) {
		case 's':
			rc = fyai_config_set(ctx, co->key, co->value);
			break;
		case 'd':
			rc = fyai_config_delete(ctx, co->key);
			break;
		case 'g':
			rc = fyai_config_get(ctx, co->key);
			break;
		default:
			rc = 0;
			break;
		}
		if (rc)
			return -1;
	}
	return 0;
}

int fyai_config_delete(struct fyai_ctx *ctx, const char *key)
{
	struct fy_generic_builder *gb = ctx->gb;
	fy_generic root;

	if (!gb) {
		fprintf(stderr, "config: no arena; run fyai init\n");
		return -1;
	}
	if (fy_generic_is_invalid(ctx->arena_config)) {
		fprintf(stderr, "config: no config in arena\n");
		return -1;
	}
	root = fy_delete_at_pathstr(gb, ctx->arena_config, key);
	if (fy_generic_is_invalid(root)) {
		fprintf(stderr, "config: delete failed\n");
		return -1;
	}
	return fyai_publish_root(ctx, root, fy_invalid, fy_invalid);
}

/* Validate an incoming config doc before it enters the immutable arena. */
static int config_doc_check(fy_generic doc, const char *origin)
{
	if (!fy_generic_is_mapping(doc)) {
		fprintf(stderr, "config: %s is not a YAML mapping\n", origin);
		return -1;
	}
	if (fyai_config_has_raw_secret(doc)) {
		fprintf(stderr,
			"config: %s carries a raw api_key; use { type: env, value: NAME }\n",
			origin);
		return -1;
	}
	return 0;
}

int fyai_config_import(struct fyai_ctx *ctx, const char *path)
{
	fy_generic doc;

	if (!ctx->gb) {
		fprintf(stderr, "config: no arena; run fyai init\n");
		return -1;
	}
	doc = fy_parse_file(ctx->gb,
			    FYOPPF_DISABLE_DIRECTORY | FYOPPF_MODE_YAML_1_2,
			    path);
	if (config_doc_check(doc, path))
		return -1;
	if (fyai_publish_root(ctx, doc, fy_invalid, fy_invalid))
		return -1;
	printf("config: imported %s\n", path);
	return 0;
}

int fyai_config_export(struct fyai_ctx *ctx, const char *path)
{
	fy_generic emitted;
	const char *text;

	if (fy_generic_is_invalid(ctx->arena_config)) {
		fprintf(stderr, "config: no config in arena\n");
		return -1;
	}
	if (!path) {
		emit_generic_to_stdout(NULL, ctx->arena_config, true);
		return 0;
	}
	emitted = fy_emit(ctx->arena_config,
			  FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_YAML_1_2 |
			  FYOPEF_STYLE_PRETTY | FYOPEF_WIDTH_INF, NULL);
	if (fy_generic_is_invalid(emitted))
		return -1;
	text = fy_castp(&emitted, "");
	if (write_text_file(path, text)) {
		fprintf(stderr, "config: cannot write %s\n", path);
		return -1;
	}
	return 0;
}

int fyai_config_edit(struct fyai_ctx *ctx)
{
	const char *tmpdir, *text;
	char tmpl[PATH_MAX];
	fy_generic emitted, doc;
	int fd;

	if (!ctx->gb) {
		fprintf(stderr, "config: no arena; run fyai init\n");
		return -1;
	}

	tmpdir = getenv("TMPDIR");
	if (!tmpdir || !*tmpdir)
		tmpdir = "/tmp";
	if (snprintf(tmpl, sizeof(tmpl), "%s/fyai-config-XXXXXX.yaml",
		     tmpdir) >= (int)sizeof(tmpl))
		return -1;
	fd = mkstemps(tmpl, 5);	/* ".yaml" suffix */
	if (fd < 0)
		return -1;

	text = "# fyai configuration\n";
	emitted = fy_invalid;
	if (fy_generic_is_valid(ctx->arena_config)) {
		emitted = fy_emit(ctx->arena_config,
				  FYOPEF_DISABLE_DIRECTORY |
				  FYOPEF_MODE_YAML_1_2 |
				  FYOPEF_STYLE_PRETTY | FYOPEF_WIDTH_INF,
				  NULL);
		if (fy_generic_is_invalid(emitted)) {
			close(fd);
			unlink(tmpl);
			return -1;
		}
		text = fy_castp(&emitted, "");
	}
	(void)!write(fd, text, strlen(text));
	close(fd);

	if (fyai_spawn_editor(tmpl)) {
		fprintf(stderr, "config: editor failed; edits kept at %s\n",
			tmpl);
		return -1;
	}

	doc = fy_parse_file(ctx->gb,
			    FYOPPF_DISABLE_DIRECTORY | FYOPPF_MODE_YAML_1_2,
			    tmpl);
	if (config_doc_check(doc, "edited config")) {
		fprintf(stderr, "config: edits kept at %s\n", tmpl);
		return -1;
	}
	if (fyai_publish_root(ctx, doc, fy_invalid, fy_invalid)) {
		fprintf(stderr, "config: edits kept at %s\n", tmpl);
		return -1;
	}
	unlink(tmpl);
	return 0;
}

void fyai_config_set_defaults(struct fyai_cfg *cfg)
{
	cfg->api_mode = FYAI_API_RESPONSES;
	cfg->api_url = NULL;
	cfg->system_prompt = DEFAULT_SYSTEM_PROMPT;
	cfg->max_tool_iterations = MAX_TOOL_LOOP_ITERATIONS;
	cfg->max_tokens = DEFAULT_MAX_TOKENS;
	cfg->temperature = DEFAULT_TEMPERATURE;
	cfg->top_logprobs = -1;
	cfg->tool_preview_lines = DEFAULT_TOOL_PREVIEW_LINES;
	cfg->markdown = true;
	cfg->stream = true;
	cfg->wire_logging = false;
	cfg->stream_logging = false;
	cfg->conversation_logging = false;
	cfg->whitewash_api_keys = true;
	cfg->markdown_mode = DEFAULT_MARKDOWN_MODE;
	cfg->color = DEFAULT_COLOR;
	cfg->theme = DEFAULT_THEME;
	cfg->code_theme = NULL;		/* NULL => the styling file's code.theme */
	cfg->markdown_theme = NULL;	/* NULL => the default shipped styling */
	cfg->markdown_style = NULL;	/* NULL => shipped styling, located at runtime */
	cfg->markdown_style_doc = fy_invalid;
	cfg->catalog = fy_invalid;
	cfg->sandbox = fy_invalid;
	cfg->cmd.id = FYAIVID_INVALID;
}

void fyai_config_cleanup(struct fyai_cfg *cfg)
{
	if (!cfg)
		return;

	fy_generic_builder_destroy(cfg->gb);
	memset(cfg, 0, sizeof(*cfg));
}

enum {
	OPT_LOGPROBS = 256,
	OPT_TOP_LOGPROBS,
	OPT_TOKEN_EXTENTS,
	OPT_NO_OBFUSCATION,
	OPT_RESPONSES,
	OPT_CHAT_COMPLETIONS,
	OPT_MESSAGES,
	OPT_BUILTIN_SHELL,
	OPT_SANDBOX,
	OPT_MARKDOWN,
	OPT_RESPONSE_CHAIN,
	OPT_NEW,
	OPT_REASONING_EFFORT,
	OPT_REASONING_SUMMARY,
	OPT_STATS,
	OPT_ANSWER,
	OPT_MARKDOWN_MODE,
	OPT_NO_MARKDOWN,
	OPT_NO_STREAM,
	OPT_COLOR,
	OPT_THEME,
	OPT_CODE_THEME,
	OPT_MARKDOWN_THEME,
	OPT_MARKDOWN_STYLE,
	OPT_SET,
	OPT_GET,
	OPT_DELETE,
	OPT_TRANSIENT,
	OPT_TEMPERATURE,
	OPT_NO_WHITEWASH,
};

static const struct option long_options[] = {
	{ "help", no_argument, NULL, 'h' },
	{ "system", required_argument, NULL, 's' },
	{ "config", required_argument, NULL, 'C' },
	{ "env", required_argument, NULL, 'e' },
	{ "model", required_argument, NULL, 'm' },
	{ "temperature", required_argument, NULL, OPT_TEMPERATURE },
	{ "reasoning-effort", required_argument, NULL, OPT_REASONING_EFFORT },
	{ "reasoning-summary", required_argument, NULL, OPT_REASONING_SUMMARY },
	{ "api-key", required_argument, NULL, 'k' },
	{ "url", required_argument, NULL, 'u' },
	{ "responses", no_argument, NULL, OPT_RESPONSES },
	{ "chat-completions", no_argument, NULL, OPT_CHAT_COMPLETIONS },
	{ "messages", no_argument, NULL, OPT_MESSAGES },
	{ "tools", no_argument, NULL, 't' },
	{ "builtin-shell", no_argument, NULL, OPT_BUILTIN_SHELL },
	{ "sandbox", no_argument, NULL, OPT_SANDBOX },
	{ "markdown", no_argument, NULL, OPT_MARKDOWN },
	{ "no-markdown", no_argument, NULL, OPT_NO_MARKDOWN },
	{ "markdown-mode", required_argument, NULL, OPT_MARKDOWN_MODE },
	{ "color", required_argument, NULL, OPT_COLOR },
	{ "theme", required_argument, NULL, OPT_THEME },
	{ "code-theme", required_argument, NULL, OPT_CODE_THEME },
	{ "markdown-theme", required_argument, NULL, OPT_MARKDOWN_THEME },
	{ "markdown-style", required_argument, NULL, OPT_MARKDOWN_STYLE },
	{ "response-chain", no_argument, NULL, OPT_RESPONSE_CHAIN },
	{ "new", no_argument, NULL, OPT_NEW },
	{ "interactive", no_argument, NULL, 'i' },
	{ "debug", no_argument, NULL, 'd' },
	{ "pretty", no_argument, NULL, 'p' },
	{ "cache-info", no_argument, NULL, 'c' },
	{ "stats", no_argument, NULL, OPT_STATS },
	{ "stream", no_argument, NULL, 'S' },
	{ "no-stream", no_argument, NULL, OPT_NO_STREAM },
	{ "logprobs", no_argument, NULL, OPT_LOGPROBS },
	{ "top-logprobs", required_argument, NULL, OPT_TOP_LOGPROBS },
	{ "token-extents", no_argument, NULL, OPT_TOKEN_EXTENTS },
	{ "no-obfuscation", no_argument, NULL, OPT_NO_OBFUSCATION },
	{ "no-whitewash", no_argument, NULL, OPT_NO_WHITEWASH },
	{ "answer", required_argument, NULL, OPT_ANSWER },
	{ "set", required_argument, NULL, OPT_SET },
	{ "get", required_argument, NULL, OPT_GET },
	{ "delete", required_argument, NULL, OPT_DELETE },
	{ "transient", no_argument, NULL, OPT_TRANSIENT },
	{ "ephemeral", no_argument, NULL, OPT_TRANSIENT },
	{ NULL, 0, NULL, 0 },
};

static const char *const effort_opts[] = {
	"minimal", "low", "medium", "high", NULL,
};
static const char *const summary_opts[] = {
	"auto", "concise", "detailed", NULL,
};
static const char *const mode_opts[] = {
	"oneshot", "line", "stream", NULL,
};
static const char *const color_opts[] = {
	"auto", "off", "on", NULL,
};
static const char *const theme_opts[] = {
	"auto", "dark", "light", NULL,
};

/*
 * The conventional API-key environment variable for a provider: its name
 * upper-cased with non-alphanumerics mapped to '_', suffixed "_API_KEY"
 * (openai -> OPENAI_API_KEY, openrouter -> OPENROUTER_API_KEY). Interned into
 * @gb (deduped, so stored once); returns NULL only when @name is too long.
 */
static const char *provider_env_key(struct fy_generic_builder *gb,
				    const char *name)
{
	char buf[128];
	size_t i;

	if (strlen(name) + sizeof("_API_KEY") > sizeof(buf))
		return NULL;
	for (i = 0; name[i]; i++)
		buf[i] = isalnum((unsigned char)name[i]) ?
			 toupper((unsigned char)name[i]) : '_';
	strcpy(buf + i, "_API_KEY");
	return fy_gb_intern_string(gb, buf);
}

/*
 * Resolve the single `model` key against the effective catalogue: strip an
 * optional provider/ pin, derive endpoint URL, API grammar and wire model id
 * from the provider's offering, validate reasoning capability, default
 * max_tokens from the model's max_output_tokens, and fall back to the
 * provider's conventional <PROVIDER>_API_KEY env var when no explicit key is
 * set. Also used to re-resolve after a mid-session /model switch (with
 * api_url/provider/api_key cleared by the caller). Returns 0 on success.
 */
int fyai_config_resolve_model(struct fyai_cfg *cfg)
{
	fy_generic catalog, cat_prov, cat_offer, cat_ep, cat_model, pinned_prov;
	const char *pmid, *slash;
	const char *env, *val;
	char *pfx;
	long long max_out;
	int i;

	/*
	 * Fall back to built-in defaults for the model and the endpoint when no
	 * config layer or CLI flag supplied them, so fyai works from any
	 * directory (not only one whose config sets them). The URL tracks the
	 * selected API grammar.
	 */
	if (!cfg->model || !*cfg->model)
		cfg->model = cfg->api_mode == FYAI_API_MESSAGES ?
			DEFAULT_ANTHROPIC_MODEL : DEFAULT_OPENAI_MODEL;

	catalog = fyai_catalog_effective(cfg->catalog, cfg->gb);

	/*
	 * A model may carry an optional "provider/" prefix that pins routing
	 * (e.g. openrouter/glm-5.2). Strip it only when the leading segment
	 * names a catalogue provider, so a provider_model_id that itself
	 * contains a slash (z-ai/glm-5.2) is left intact.
	 */
	pinned_prov = fy_invalid;
	slash = cfg->model ? strchr(cfg->model, '/') : NULL;
	if (slash) {
		pfx = strndup(cfg->model, slash - cfg->model);

		if (!pfx)
			return -1;
		pinned_prov = fyai_catalog_provider(catalog, pfx);
		if (fy_generic_is_valid(pinned_prov))
			cfg->model = fy_gb_intern_string(cfg->gb, slash + 1);
		free(pfx);
	}

	/*
	 * Catalog-derived resolution: when nothing supplied an endpoint, look
	 * the model up in the effective catalogue (pinned to the prefixed
	 * provider when given, else the canonical provider) and derive provider
	 * URL, API grammar and wire model id from its offering. Unknown models
	 * fall through to the static defaults below.
	 */
	if (!cfg->api_url || !*cfg->api_url) {
		if (fy_generic_is_valid(pinned_prov)) {
			cat_prov = pinned_prov;
			fyai_catalog_offering(cat_prov, cfg->model, &cat_offer);
		} else {
			cat_prov = fyai_catalog_provider_for_model(catalog,
						cfg->model, &cat_offer);
		}
		cat_ep = fyai_catalog_endpoint(cat_prov, cfg->api_mode);
		if (fy_generic_is_invalid(cat_ep) &&
		    fy_generic_is_valid(cat_prov)) {
			/* provider does not speak the current grammar: take
			 * the first one it offers that fyai supports */
			for (i = 0; i < 3 && fy_generic_is_invalid(cat_ep); i++) {
				cat_ep = fyai_catalog_endpoint(cat_prov,
						(enum fyai_api_mode)i);
				if (fy_generic_is_valid(cat_ep))
					cfg->api_mode = (enum fyai_api_mode)i;
			}
		}
		if (fy_generic_is_valid(cat_ep)) {
			cfg->api_url = fy_gb_intern_string(cfg->gb,
					fy_sprintfa("%s%s",
						fy_get(cat_prov, "root_url", ""),
						fy_get(cat_ep, "endpoint", "")));
			cfg->provider = fy_gb_intern_string(cfg->gb,
						fy_get(cat_prov, "name", ""));
			pmid = fy_get(cat_offer, "provider_model_id", "");
			if (*pmid)
				cfg->model = fy_gb_intern_string(cfg->gb, pmid);
		}
	} else if (fy_generic_is_valid(pinned_prov)) {
		/* Endpoint already fixed (e.g. --url), but honour the pin for
		 * display, turn metadata and the api-key env mapping. */
		cfg->provider = fy_gb_intern_string(cfg->gb,
					fy_get(pinned_prov, "name", ""));
	}

	/*
	 * Capability validation against the catalogue (skipped entirely for
	 * models it does not know).
	 */
	cat_model = fyai_catalog_resolved_model(catalog, cfg->model);
	if (fy_generic_is_valid(cat_model)) {
		if (((cfg->reasoning_effort && *cfg->reasoning_effort) ||
		     (cfg->reasoning_summary && *cfg->reasoning_summary)) &&
		    fyai_model_supports_temperature(cat_model)) {
			fprintf(stderr,
				"model '%s' is not reasoning-capable (catalog)\n",
				cfg->model);
			return -1;
		}
		if (cfg->max_tokens == DEFAULT_MAX_TOKENS) {
			max_out = fy_get(cat_model, "max_output_tokens", 0LL);
			if (max_out > 0)
				cfg->max_tokens = (int)max_out;
		}
	}

	/*
	 * A model the catalogue does not know still resolves to the built-in
	 * default endpoint below (OpenAI/Anthropic per the selected grammar);
	 * give it the conventional provider for that grammar so the
	 * <PROVIDER>_API_KEY fallback and turn metadata work - otherwise a valid
	 * OpenAI-shaped model typed without a catalogue entry (e.g. gpt-4o-mini)
	 * fails with a misleading "no API key".
	 */
	if (!cfg->provider || !*cfg->provider)
		cfg->provider = cfg->api_mode == FYAI_API_MESSAGES ?
			"anthropic" : "openai";

	/*
	 * With no explicit api_key (from --api-key or a config `api_key` env
	 * mapping), fall back to the provider's conventional environment
	 * variable, e.g. OPENAI_API_KEY / ANTHROPIC_API_KEY / OPENROUTER_API_KEY.
	 */
	if ((!cfg->api_key || !*cfg->api_key) && cfg->provider && *cfg->provider) {
		env = provider_env_key(cfg->gb, cfg->provider);
		if (env) {
			val = getenv(env);
			if (val && *val)
				cfg->api_key = fy_gb_intern_string(cfg->gb, val);
		}
	}

	if (!cfg->api_url || !*cfg->api_url) {
		switch (cfg->api_mode) {
		case FYAI_API_CHAT_COMPLETIONS:
			cfg->api_url = OPENAI_CHAT_COMPLETIONS_URL;
			break;
		case FYAI_API_MESSAGES:
			cfg->api_url = ANTHROPIC_MESSAGES_URL;
			break;
		case FYAI_API_RESPONSES:
		default:
			cfg->api_url = OPENAI_RESPONSES_URL;
			break;
		}
	}

	return 0;
}

/*
 * Features with no Messages-API mapping yet: extended thinking (the
 * reasoning options), the OpenAI built-in shell tool, and logprobs. Reject
 * them up front rather than sending a request the API 400s. No-op unless the
 * Messages API is selected.
 */
int fyai_config_messages_gate(struct fyai_cfg *cfg)
{
	if (cfg->api_mode != FYAI_API_MESSAGES)
		return 0;
	if ((cfg->reasoning_effort && *cfg->reasoning_effort) ||
	    (cfg->reasoning_summary && *cfg->reasoning_summary)) {
		fprintf(stderr, "reasoning options are not supported "
			"with the Messages API yet\n");
		return -1;
	}
	/*
	 * The OpenAI built-in shell tool has no Messages analogue. Config
	 * commonly enables it globally, so downgrade to the function shell
	 * tool instead of failing the run.
	 */
	if (cfg->enable_builtin_shell) {
		fprintf(stderr, "note: the built-in shell tool is not "
			"available with the Messages API; using the "
			"function shell tool\n");
		cfg->enable_builtin_shell = false;
		cfg->enable_tools = true;
	}
	if (cfg->logprobs || cfg->top_logprobs >= 0) {
		fprintf(stderr, "logprobs are not supported with the "
			"Messages API\n");
		return -1;
	}
	return 0;
}

/*
 * Fold every pending --set into @cfg as the highest-precedence layer, so this
 * invocation sees the edited values (deletes and gets act on the arena at
 * storage-open time instead). Builds a single delta mapping from the set paths
 * and overlays it via apply_config.
 */
static int apply_config_set_ops(struct fyai_cfg *cfg)
{
	struct fyai_config_op *co;
	fy_generic delta, v;
	size_t i;

	delta = fy_map_empty;
	for (i = 0; i < cfg->config_op_count; i++) {
		co = &cfg->config_ops[i];
		if (co->op != 's')
			continue;
		v = config_parse_value(cfg->gb, co->value);
		if (fy_generic_is_invalid(v)) {
			fprintf(stderr, "--set %s: cannot parse value '%s'\n",
				co->key, co->value);
			return -1;
		}
		delta = fy_set_at_pathstr(cfg->gb, delta, co->key, v);
		if (fy_generic_is_invalid(delta)) {
			fprintf(stderr, "--set %s: failed\n", co->key);
			return -1;
		}
	}
	return apply_config(cfg, delta);
}

int fyai_config_setup(struct fyai_cfg *cfg, int argc, char *argv[])
{
	struct fy_generic_builder_cfg gb_cfg;
	struct fyai_config_op *co;
	const char *cli_config, *cli_env, *cli_color;
	struct fyai_last_turn lt = { };
	bool got_lt = false;
	bool pin;
	int opt, rc;
	char *endp, *def_arena_dir = NULL;
	long v;
	double d;
	const char *verb = NULL;
	bool stdin_prompt;
	char *prompt = NULL;
	int ret = -1;
	char tmp_prompt[16];
	char *tmp_argv[2];
	char *eq, *k;

	if (!cfg)
		return -1;

	memset(cfg, 0, sizeof(*cfg));

	memset(&gb_cfg, 0, sizeof(gb_cfg));
	gb_cfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	cfg->gb = fy_generic_builder_create(&gb_cfg);
	if (!cfg->gb)
		goto err_out;

	fyai_config_set_defaults(cfg);

	cli_config = find_cli_option(argc, argv, "--config", 'C');
	cli_env = find_cli_option(argc, argv, "--env", 'e');
	cli_color = find_cli_option(argc, argv, "--color", 0);

	rc = fyai_config_load(cfg, cli_config, cli_env);
	if (rc)
		goto err_out;

	if (cli_color)
		cfg->color = cli_color;

	/*
	 * Carry the conversation's settings over from its last turn (unless
	 * --new) so a continuation defaults to them; the command line still
	 * overrides below.
	 */
	if (!has_cli_flag(argc, argv, "--new")) {
		fyai_peek_last_turn(cfg, &lt);
		got_lt = true;

		if (lt.has_temperature)
			cfg->temperature = lt.temperature;
		/*
		 * The conversation continues on the model/provider/grammar its
		 * last turn used, so /model and /api switches stick. The model
		 * is pinned to the recorded provider (provider/model form) so
		 * resolution stays on that provider's offering; the resolver
		 * strips the prefix only when the catalogue knows the provider.
		 */
		if (lt.model && *lt.model) {
			pin = lt.provider && *lt.provider &&
			      fy_generic_is_valid(fyai_catalog_provider(
					fyai_catalog_effective(cfg->catalog,
							       cfg->gb),
					lt.provider));
			cfg->model = fy_gb_intern_string(cfg->gb, pin ?
					fy_sprintfa("%s/%s", lt.provider,
						    lt.model) :
					lt.model);
			cfg->api_url = NULL;
			cfg->provider = NULL;
			cfg->max_tokens = DEFAULT_MAX_TOKENS;
			if (!cfg->api_key_explicit)
				cfg->api_key = NULL;
		}
		if (lt.api) {
			if (!strcmp(lt.api, "chat-completions"))
				cfg->api_mode = FYAI_API_CHAT_COMPLETIONS;
			else if (!strcmp(lt.api, "messages"))
				cfg->api_mode = FYAI_API_MESSAGES;
			else if (!strcmp(lt.api, "responses"))
				cfg->api_mode = FYAI_API_RESPONSES;
		}
		if (lt.reasoning_effort && *lt.reasoning_effort)
			cfg->reasoning_effort = fy_gb_intern_string(cfg->gb, lt.reasoning_effort);
		if (lt.reasoning_summary && *lt.reasoning_summary)
			cfg->reasoning_summary = fy_gb_intern_string(cfg->gb, lt.reasoning_summary);

		fyai_last_turn_cleanup(&lt);
		got_lt = false;
	}

	optind = 0;
	optarg = NULL;

	/* '+' stops parsing at the first non-option (the verb or prompt). */
	while ((opt = getopt_long(argc, argv, "+hs:C:e:m:k:u:tipdcS",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			fyai_usage(stdout, "fyai", cfg->color);
			ret = 1;
			goto out;
		case 's':
			cfg->system_prompt = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case 'C':
		case 'e':
			/* Consumed before config load by find_cli_option(). */
			break;
		case 'm':
			cfg->model = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_TEMPERATURE:
			errno = 0;
			d = strtod(optarg, &endp);
			if (errno || *endp) {
				fprintf(stderr, "invalid temperature: %s\n", optarg);
				goto err_out;
			}
			cfg->temperature = (float)d;
			break;
		case 'k':
			cfg->api_key = fy_gb_intern_string(cfg->gb, optarg);
			cfg->api_key_explicit = true;
			break;
		case 'u':
			cfg->api_url = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_RESPONSES:
			cfg->api_mode = FYAI_API_RESPONSES;
			break;
		case OPT_CHAT_COMPLETIONS:
			cfg->api_mode = FYAI_API_CHAT_COMPLETIONS;
			break;
		case OPT_MESSAGES:
			cfg->api_mode = FYAI_API_MESSAGES;
			break;
		case 't':
			cfg->enable_tools = true;
			break;
		case OPT_BUILTIN_SHELL:
			cfg->enable_builtin_shell = true;
			break;
		case OPT_SANDBOX:
			cfg->enable_sandbox = true;
			break;
		case OPT_MARKDOWN:
			cfg->markdown = true;
			break;
		case OPT_NO_MARKDOWN:
			cfg->markdown = false;
			break;
		case OPT_RESPONSE_CHAIN:
			cfg->response_chain = true;
			break;
		case OPT_NEW:
			cfg->new_conversation = true;
			break;
		case 'i':
			cfg->interactive = true;
			break;
		case 'd':
			cfg->debug++;
			break;
		case 'p':
			cfg->pretty = true;
			break;
		case 'c':
			cfg->cache_info = true;
			break;
		case 'S':
			cfg->stream = true;
			break;
		case OPT_NO_STREAM:
			cfg->stream = false;
			break;
		case OPT_LOGPROBS:
			cfg->logprobs = true;
			break;
		case OPT_TOP_LOGPROBS:
			errno = 0;
			v = strtol(optarg, &endp, 10);
			if (errno || *endp || v < 0 || v > 20) {
				fprintf(stderr, "invalid top-logprobs: %s\n", optarg);
				goto err_out;
			}
			cfg->logprobs = true;
			cfg->top_logprobs = (int)v;
			break;
		case OPT_TOKEN_EXTENTS:
			cfg->token_extents = true;
			break;
		case OPT_STATS:
			cfg->stats = true;
			break;
		case OPT_REASONING_EFFORT:
			if (str_in_set(optarg, effort_opts) < 0) {
				fprintf(stderr,
					"invalid reasoning effort '%s' (minimal|low|medium|high)\n",
					optarg);
				goto err_out;
			}
			cfg->reasoning_effort = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_REASONING_SUMMARY:
			if (str_in_set(optarg, summary_opts) < 0) {
				fprintf(stderr,
					"invalid reasoning summary '%s' (auto|concise|detailed)\n",
					optarg);
				goto err_out;
			}
			cfg->reasoning_summary = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_NO_OBFUSCATION:
			cfg->no_obfuscation = true;
			break;
		case OPT_NO_WHITEWASH:
			cfg->whitewash_api_keys = false;
			break;
		case OPT_MARKDOWN_MODE:
			if (str_in_set(optarg, mode_opts) < 0) {
				fprintf(stderr,
					"invalid markdown mode '%s' (oneshot|line|stream)\n",
					optarg);
				goto err_out;
			}
			cfg->markdown_mode = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_COLOR:
			cfg->color = optarg;
			if (str_in_set(optarg, color_opts) < 0) {
				fprintf(stderr, "invalid color '%s' (auto|off|on)\n",
					optarg);
				goto err_out;
			}
			cfg->color = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_THEME:
			if (str_in_set(optarg, theme_opts) < 0) {
				fprintf(stderr, "invalid theme '%s' (auto|dark|light)\n",
					optarg);
				return -1;
			}
			cfg->theme = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_CODE_THEME:
			cfg->code_theme = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_MARKDOWN_THEME:
			cfg->markdown_theme = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_MARKDOWN_STYLE:
			cfg->markdown_style = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_ANSWER:
			if (cfg->answer_count >= ARRAY_SIZE(cfg->answers)) {
				fprintf(stderr, "too many answers, max %zu\n", ARRAY_SIZE(cfg->answers));
				goto err_out;
			}
			cfg->answers[cfg->answer_count++] = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_SET:
		case OPT_GET:
		case OPT_DELETE:
			if (cfg->config_op_count >= ARRAY_SIZE(cfg->config_ops)) {
				fprintf(stderr, "too many --set/--get/--delete ops, max %zu\n",
					ARRAY_SIZE(cfg->config_ops));
				goto err_out;
			}
			co = &cfg->config_ops[cfg->config_op_count];
			eq = opt == OPT_SET ? strchr(optarg, '=') : NULL;

			co->value = NULL;
			if (opt == OPT_SET && eq) {
				k = strndup(optarg, eq - optarg);
				if (!k)
					goto err_out;
				co->key = fy_gb_intern_string(cfg->gb, k);
				free(k);
				co->value = fy_gb_intern_string(cfg->gb, eq + 1);
			} else if (opt == OPT_SET) {
				co->key = fy_gb_intern_string(cfg->gb, optarg);
				if (optind >= argc) {
					fprintf(stderr,
						"--set %s: missing value\n", optarg);
					goto err_out;
				}
				co->value = fy_gb_intern_string(cfg->gb,
							argv[optind++]);
			} else {
				co->key = fy_gb_intern_string(cfg->gb, optarg);
			}
			co->op = opt == OPT_SET ? 's' :
				 opt == OPT_GET ? 'g' : 'd';
			cfg->config_op_count++;
			break;
		case OPT_TRANSIENT:
			cfg->transient = true;
			break;

		default:
			fyai_usage(stderr, "fyai", cfg->color);
			ret = 0;
			goto out;
		}
	}

	/*
	 * Fold pending --set edits into this run before model/theme resolution,
	 * so `--set model=X` (or display/theme, etc.) takes effect immediately.
	 * They also persist at storage-open time (see fyai_apply_config_ops).
	 */
	if (apply_config_set_ops(cfg))
		goto err_out;

	/*
	 * Resolve theme=auto to a concrete dark/light once, so the render path
	 * never probes per chunk. Only probe when markdown is actually rendered
	 * in colour: the probe reads /dev/tty, which must not run (and possibly
	 * swallow typed-ahead input) when we are not even rendering markdown.
	 */
	if (cfg->theme && !strcmp(cfg->theme, "auto"))
		cfg->theme = (cfg->markdown && markdown_color_enabled(cfg->color)) ?
				terminal_detect_theme() : "dark";

	/* Load the shipped markdown styling (element colours + fenced-code theme)
	 * now that the theme is concrete; applied to every subsequent render. */
	if (cfg->markdown)
		fyai_markdown_load_style(cfg);

	if (fyai_config_resolve_model(cfg))
		goto err_out;

	/* if no arena dir, use the default */
	if (!cfg->arena_dir) {
		def_arena_dir = fyai_default_arena_dir();
		cfg->arena_dir = fy_gb_intern_string(cfg->gb, def_arena_dir);
		free(def_arena_dir);
		def_arena_dir = NULL;
	}

	/* A known verb at optind dispatches; otherwise it's a prompt. */
	verb = optind < argc && fyai_is_verb(argv[optind]) ? argv[optind] : NULL;

	if (!verb && optind >= argc && cfg->config_op_count) {
		/*
		 * A bare --set/--get/--delete run (no verb, no prompt) is a
		 * config-only invocation: dispatch the config verb as a no-op so
		 * storage opens without requiring an API key; the global ops run
		 * in fyai_run.
		 */
		cfg->cmd.id = FYAIVID_CONFIG;
		cfg->cmd.args.config.type = FYAICT_NOOP;
		ret = 0;
	} else if (verb) {
		cfg->cmd.id = fyai_get_verb_id(verb);
		argv += optind;
		argc -= optind;
	} else {
		stdin_prompt = (!cfg->interactive && optind >= argc && !terminal_is_tty(STDIN_FILENO)) ||
			       (optind < argc && argc - optind == 1 && !strcmp(argv[optind], "-"));

		/* No prompt and a terminal on stdin: drop into interactive mode. */
		if (optind >= argc && !stdin_prompt)
			cfg->interactive = true;

		prompt = NULL;
		if (stdin_prompt) {
			prompt = read_all_stdin();
			if (!prompt) {
				fprintf(stderr, "failed to read prompt from stdin\n");
				goto err_out;
			}
		} else if (optind < argc) {
			prompt = join_args(argc - optind, argv + optind);
			if (!prompt) {
				fprintf(stderr, "Out of memory\n");
				goto err_out;;
			}
		}
		cfg->prompt = fy_gb_intern_string(cfg->gb, prompt ? prompt : "");
		free(prompt);
		prompt = NULL;

		cfg->cmd.id = FYAIVID_PROMPT;

		strncpy(tmp_prompt, "prompt", sizeof(tmp_prompt));
		tmp_argv[0] = tmp_prompt;
		tmp_argv[1] = NULL; 

		argc = 1;
		argv = tmp_argv;
	}

	/*
	 * Features with no Messages-API mapping yet: extended thinking (the
	 * reasoning options), the OpenAI built-in shell tool, and logprobs.
	 * Reject them up front rather than sending a request the API 400s.
	 * Only request-making verbs are affected (this runs after verb
	 * dispatch so the verb flags are known); config verbs must be able
	 * to inspect and fix such a config.
	 */
	if (fyai_cfg_makes_requests(cfg) && fyai_config_messages_gate(cfg))
		goto err_out;

	if (ret < 0) {
		ret = fyai_configure(cfg, argc, argv);
		if (ret)
			goto err_out;
	}

	return ret;

out:
	if (got_lt)
		fyai_last_turn_cleanup(&lt);
	fyai_config_cleanup(cfg);
	return ret;

err_out:
	ret = -1;
	goto out;
}
