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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_schema.h"
#include "fyai_secret.h"
#include "fyai_terminal.h"
#include "fyai_markdown.h"
#include "fyai_storage.h"
#include "commands.h"
#include "utils.h"

/* FYAI_EMBEDDED_CONFIG_SCHEMA[] / FYAI_EMBEDDED_CONFIG_SCHEMA_LEN - the
 * vendored data/config.schema.yaml, generated at configure time. */
#include "embedded_config_schema.inc"

/* Environment variables fyai reads directly, always allowed from --env. */
static const char *const fyai_env_direct[] = {
	"OPENAI_API_KEY", "OPENAI_MODEL", "OPENAI_URL",
	"ANTHROPIC_API_KEY",
};

static int config_bad_value(const char *key, fy_generic value,
			    fy_generic choices)
{
	fprintf(stderr, "config: invalid %s '%s' (%s)\n",
		key, fy_cast(fy_convert(value, FYGT_STRING), ""),
		fy_cast(choices, ""));
	return -1;
}

static bool config_contains(fy_generic values, fy_generic value)
{
	return fy_cast(fy_contains(values, value), (_Bool)false);
}

static int config_validate_enum(fy_generic root, const char *key,
				fy_generic values, fy_generic choices)
{
	fy_generic v;

	v = fy_get(root, key);
	if (fy_generic_is_invalid(v))
		return 0;
	return config_contains(values, v) ? 0 :
	       config_bad_value(key, v, choices);
}

#define CONFIG_VALIDATE_ENUM(_root, _key, ...) \
	config_validate_enum((_root), (_key), \
			     fy_sequence(__VA_ARGS__), \
			     fy_join("|", __VA_ARGS__))

static int config_validate_api(fy_generic root)
{
	fy_generic v;

	v = fy_get(root, "api");
	if (fy_generic_is_invalid(v))
		return 0;
	if (config_contains(fy_sequence("responses", "messages", "chat",
					"chat-completions"), v))
		return 0;
	fprintf(stderr,
		"config: invalid api '%s' (responses|chat-completions|messages)\n",
		fy_cast(fy_convert(v, FYGT_STRING), ""));
	return -1;
}

static bool config_has_command_ops(struct fyai_cfg *cfg);
static int config_report_commit(fy_generic report, fy_generic *docp);
static fy_generic config_doc_mirror_key(struct fy_generic_builder *gb,
					fy_generic config_doc, fy_generic root,
					const char *key);

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

static int resolve_secret(struct fyai_cfg *cfg, const char **out, fy_generic v)
{
	const char *name;
	const char *value;
	const char *key_name;
	char *secret = NULL;
	size_t len = 0;
	int rc;

	if (fy_generic_is_invalid(v))
		return -1;

	name = fy_get(v, "value", "");
	if (!*name)
		return -1;
	if (fy_equal(fy_get(v, "type"), "secret")) {
		key_name = fy_sprintfa("fyai:%s", name);
		rc = fyai_secret_kernel_get(key_name, &secret, &len);
		if (rc != FYAI_SECRET_OK)
			return -1;
		*out = fy_gb_intern_string(cfg->gb, secret);
		fyai_secret_clear_and_free(&secret, &len);
		return *out && **out ? 0 : -1;
	}
	if (fy_not_equal(fy_get(v, "type"), "env"))
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
	fy_generic v, sb, tbv, secret_ref;

	if (fy_generic_is_invalid(root))
		return 0;

	secret_ref = fy_get(root, "api_key");
	if (fy_generic_is_mapping(secret_ref)) {
		if (fy_equal(fy_get(secret_ref, "type"), "auto")) {
			cfg->api_key = NULL;
			cfg->api_key_auto = true;
			cfg->api_key_explicit = false;
		} else {
			cfg->api_key = NULL;
			cfg->api_key_auto = false;
			cfg->api_key_explicit = true;
			(void)resolve_secret(cfg, &cfg->api_key, secret_ref);
		}
	}

	if (config_validate_api(root))
		return -1;

	v = fy_get(root, "model");
	if (!fy_generic_is_invalid(v)) {
		cfg->model = fy_cast(v, cfg->model);
		cfg->model_explicit = true;
	}
	v = fy_get(root, "auth");
	if (fy_generic_is_invalid(v)) {
		/* Absent keys preserve the lower-precedence layer/default. */
	} else if (fy_equal(v, "api-key"))
		cfg->auth_mode = FYAI_AUTH_API_KEY;
	else if (fy_equal(v, "chatgpt"))
		cfg->auth_mode = FYAI_AUTH_CHATGPT;
	else if (fy_equal(v, "auto"))
		cfg->auth_mode = FYAI_AUTH_AUTO;
	else {
		fprintf(stderr, "config: auth must be auto, api-key, or chatgpt\n");
		return -1;
	}
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
	cfg->whitewash_api_keys = apply_bool(root, "whitewash_api_keys",
					     cfg->whitewash_api_keys);
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
		if (!fy_generic_is_mapping(v)) {
			fprintf(stderr, "config: reasoning must be a mapping\n");
			return -1;
		}
		if (CONFIG_VALIDATE_ENUM(v, "effort",
					 "minimal", "low", "medium", "high") ||
		    CONFIG_VALIDATE_ENUM(v, "summary",
					 "auto", "concise", "detailed"))
			return -1;
		cfg->reasoning_effort = fy_get(v, "effort",
					       cfg->reasoning_effort);
		cfg->reasoning_summary = fy_get(v, "summary",
						cfg->reasoning_summary);
	}

	v = fy_get(root, "api");
	if (fy_equal(v, "chat-completions") || fy_equal(v, "chat"))
		cfg->api_mode = FYAI_API_CHAT_COMPLETIONS;
	else if (fy_equal(v, "responses"))
		cfg->api_mode = FYAI_API_RESPONSES;
	else if (fy_equal(v, "messages"))
		cfg->api_mode = FYAI_API_MESSAGES;

	/* Stylistic options live in the display: group (the only form). */
	v = fy_get(root, "display");
	if (!fy_generic_is_invalid(v)) {
		if (!fy_generic_is_mapping(v)) {
			fprintf(stderr, "config: display must be a mapping\n");
			return -1;
		}
		if (CONFIG_VALIDATE_ENUM(v, "markdown_mode",
					 "oneshot", "line", "stream") ||
		    CONFIG_VALIDATE_ENUM(v, "color",
					 "auto", "off", "on") ||
		    CONFIG_VALIDATE_ENUM(v, "theme",
					 "auto", "dark", "light"))
			return -1;
		cfg->markdown_mode = fy_get(v, "markdown_mode",
					    cfg->markdown_mode);
		cfg->color = fy_get(v, "color", cfg->color);
		cfg->theme = fy_get(v, "theme", cfg->theme);
		cfg->code_theme = fy_get(v, "code_theme", cfg->code_theme);
		cfg->markdown_theme = fy_get(v, "markdown_theme",
					     cfg->markdown_theme);
		/*
		 * Intern the separators: they are read back only at render time
		 * (well after this doc may be freed), so a raw fy_get pointer to a
		 * short inline string would dangle (see CLAUDE.md fy_castp note).
		 */
		cfg->turn_separator = fy_gb_intern_string(cfg->gb,
			fy_get(v, "turn_separator", cfg->turn_separator));
		cfg->tool_separator = fy_gb_intern_string(cfg->gb,
			fy_get(v, "tool_separator", cfg->tool_separator));
		cfg->section_separator = fy_gb_intern_string(cfg->gb,
			fy_get(v, "section_separator", cfg->section_separator));
		cfg->prompt_marker = fy_gb_intern_string(cfg->gb,
			fy_get(v, "prompt", cfg->prompt_marker));
		cfg->prompt_top = fy_gb_intern_string(cfg->gb,
			fy_get(v, "prompt_top", cfg->prompt_top));
		cfg->prompt_bottom = fy_gb_intern_string(cfg->gb,
			fy_get(v, "prompt_bottom", cfg->prompt_bottom));
		/* Table-border override (int, so no string-lifetime concern). */
		tbv = fy_get(v, "table_border");
		if (fy_equal(tbv, "grid"))
			cfg->table_border = 1;
		else if (fy_equal(tbv, "none"))
			cfg->table_border = 2;
		else if (fy_equal(tbv, "theme"))
			cfg->table_border = 0;
		cfg->tool_preview_lines = fy_get(v, "tool_preview_lines",
						cfg->tool_preview_lines);
		cfg->markdown = apply_bool(v, "markdown", cfg->markdown);
		cfg->stream = apply_bool(v, "stream", cfg->stream);
		cfg->thinking = apply_bool(v, "thinking", cfg->thinking);
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
 * Join @path onto the process cwd when relative (matching the fopen()
 * semantics read_text_file/write_text_file actually use), then lexically
 * collapse "." and ".." components - no stat()/realpath(), so it works for a
 * path that does not exist yet (write_file creating a new file). Returns a
 * malloc'd absolute path, or NULL on allocation failure.
 */
static char *lexical_absolute(const char *path)
{
	char *joined, *out, *copy, *tok, *save;
	char *comps[PATH_MAX / 2];
	char cwd[PATH_MAX];
	size_t ncomp = 0, len, i;

	if (path[0] != '/') {
		if (!getcwd(cwd, sizeof(cwd)))
			return NULL;
		if (asprintf(&joined, "%s/%s", cwd, path) < 0)
			return NULL;
	} else {
		joined = strdup(path);
		if (!joined)
			return NULL;
	}

	copy = joined;
	for (tok = strtok_r(copy, "/", &save); tok;
	     tok = strtok_r(NULL, "/", &save)) {
		if (!strcmp(tok, "."))
			continue;
		if (!strcmp(tok, "..")) {
			if (ncomp > 0)
				ncomp--;
			continue;
		}
		if (ncomp < ARRAY_SIZE(comps))
			comps[ncomp++] = tok;
	}

	len = 1;
	for (i = 0; i < ncomp; i++)
		len += strlen(comps[i]) + 1;
	out = malloc(len);
	if (!out) {
		free(joined);
		return NULL;
	}
	out[0] = '\0';
	for (i = 0; i < ncomp; i++) {
		strcat(out, "/");
		strcat(out, comps[i]);
	}
	if (!ncomp)
		strcpy(out, "/");
	free(joined);
	return out;
}

bool fyai_arena_path_denied(const char *path)
{
	char *root, *abs, *fyai_dir;
	bool denied;
	size_t len;

	if (!path || !*path)
		return false;

	root = fyai_discover_project_root();
	if (!root)
		return false;

	abs = lexical_absolute(path);
	if (!abs) {
		free(root);
		return false;
	}

	if (asprintf(&fyai_dir, "%s/.fyai", root) < 0) {
		free(root);
		free(abs);
		return false;
	}

	len = strlen(fyai_dir);
	denied = !strcmp(abs, fyai_dir) ||
		 (!strncmp(abs, fyai_dir, len) && abs[len] == '/');

	free(root);
	free(abs);
	free(fyai_dir);
	return denied;
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

/*
 * Deep-merge @over onto @base: mappings merge key-wise recursively, any
 * other type (scalars, sequences) is replaced wholesale by @over. Either
 * side may be fy_invalid, meaning "absent".
 */
static fy_generic config_merge(struct fy_generic_builder *gb,
			       fy_generic base, fy_generic over)
{
	fy_generic key, val, bval, out;
	size_t i, n;

	if (fy_generic_is_invalid(over))
		return base;
	if (fy_generic_is_invalid(base) ||
	    !fy_generic_is_mapping(base) || !fy_generic_is_mapping(over))
		return over;

	out = base;
	n = fy_generic_mapping_get_pair_count(over);
	for (i = 0; i < n; i++) {
		key = fy_generic_mapping_get_at_key(over, i);
		val = fy_generic_mapping_get_at_value(over, i);
		bval = fy_get(out, key, fy_invalid);
		if (fy_generic_is_mapping(bval) && fy_generic_is_mapping(val))
			val = config_merge(gb, bval, val);
		out = fy_assoc(gb, out, key, val);
	}
	return out;
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

	/*
	 * The single configuration source: the arena config is the base (the
	 * user file only bootstraps a project with no arena config yet), with
	 * an explicit --config merged on top. One document, one apply pass;
	 * `config effective` emits it verbatim.
	 */
	cfg->config_doc = config_merge(gb,
				       fy_generic_is_valid(root_repo) ?
						root_repo : root_user,
				       root_explicit);
	if (apply_config(cfg, cfg->config_doc))
		return -1;

	return 0;
}

/*
 * The effective config is the merged document itself - the single source -
 * not a re-synthesized view of the struct fields. Catalog-derived values
 * (provider, endpoint, max_tokens) are deliberately absent: they are
 * derivations, not configuration.
 */
int fyai_config_show(struct fyai_cfg *cfg)
{
	if (fy_generic_is_invalid(cfg->config_doc)) {
		fprintf(stderr,
			"config: no configuration; run fyai init or fyai config import\n");
		return -1;
	}
	emit_generic_to_stdout(NULL, cfg->config_doc, true);
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
	fy_generic report;
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
	report = fyai_config_validate_report(ctx->cfg, root, "config");
	if (config_report_commit(report, &root)) {
		return -1;
	}
	/* The merged doc tracks the edit (validated form, including any
	 * re-derived catalog: block) so the effective view and this run
	 * stay on the single source. */
	ctx->cfg->config_doc = config_doc_mirror_key(gb, ctx->cfg->config_doc,
						     root, key);
	ctx->cfg->config_doc = config_doc_mirror_key(gb, ctx->cfg->config_doc,
						     root, "catalog");
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
	fy_generic report;
	fy_generic root, v;
	bool dirty;
	size_t i;

	if (!config_has_command_ops(cfg))
		return 0;
	if (!ctx->gb) {
		fprintf(stderr,
			"--set/--get/--delete need an arena; run fyai init\n");
		return -1;
	}
	root = fy_generic_is_valid(ctx->arena_config) ?
	       ctx->arena_config : fy_map_empty;
	dirty = false;
	for (i = 0; i < cfg->config_op_count; i++) {
		co = &cfg->config_ops[i];
		if (!co->command)
			continue;
		switch (co->op) {
		case 's':
			v = config_parse_value(ctx->gb, co->value);
			if (fy_generic_is_invalid(v)) {
				fprintf(stderr, "config: cannot parse value '%s'\n",
					co->value);
				return -1;
			}
			root = fy_set_at_pathstr(ctx->gb, root, co->key, v);
			if (fy_generic_is_invalid(root)) {
				fprintf(stderr, "config: set failed\n");
				return -1;
			}
			dirty = co->persistent || dirty;
			break;
		case 'd':
			root = fy_delete_at_pathstr(ctx->gb, root, co->key);
			if (fy_generic_is_invalid(root)) {
				fprintf(stderr, "config: delete failed\n");
				return -1;
			}
			dirty = co->persistent || dirty;
			break;
		case 'g':
			v = fy_get_at_pathstr(ctx->gb, root, co->key);
			if (fy_generic_is_invalid(v)) {
				fprintf(stderr, "config: key '%s' not set\n",
					co->key);
				return -1;
			}
			(void)fy_emit(v, FYOPEF_DISABLE_DIRECTORY |
					 FYOPEF_OUTPUT_TYPE_STDOUT |
					 FYOPEF_MODE_YAML_1_2 |
					 FYOPEF_STYLE_ONELINE |
					 FYOPEF_WIDTH_INF, NULL);
			break;
		default:
			break;
		}
	}
	if (dirty) {
		report = fyai_config_validate_report(cfg, root, "config");
		if (config_report_commit(report, &root))
			return -1;
		if (fyai_publish_root(ctx, root, fy_invalid, fy_invalid))
			return -1;
	}
	return 0;
}

static int config_doc_finalize(struct fyai_cfg *cfg)
{
	struct fyai_cfg tmp;

	tmp = *cfg;
	if (tmp.theme && !strcmp(tmp.theme, "auto"))
		tmp.theme = "dark";
	if (fyai_config_resolve_model(&tmp))
		return -1;
	if (fyai_config_messages_gate(&tmp))
		return -1;
	return 0;
}

static fy_generic config_problem_add(struct fy_generic_builder *gb,
				       fy_generic problems,
				       const char *fmt, ...)
{
	va_list ap;
	char *msg = NULL;
	fy_generic out;

	va_start(ap, fmt);
	(void)vasprintf(&msg, fmt, ap);
	va_end(ap);
	if (!msg)
		return problems;
	out = fy_append(gb, problems, msg);
	free(msg);
	return out;
}

static bool config_problem_any(fy_generic problems)
{
	fy_generic problem;

	fy_foreach(problem, problems)
		return true;
	return false;
}

static fy_generic config_change_add(struct fy_generic_builder *gb,
				    fy_generic changes, const char *path,
				    const char *action, fy_generic before,
				    fy_generic after)
{
	return fy_append(gb, changes,
			 fy_mapping("path", path,
				    "action", action,
				    "before", fy_generic_is_valid(before) ?
					      before : fy_null,
				    "after", fy_generic_is_valid(after) ?
					     after : fy_null));
}

/*
 * Parse the `api` intent key into an enum, defaulting when absent/unknown.
 */
static enum fyai_api_mode config_doc_api_mode(fy_generic doc,
					      enum fyai_api_mode dflt)
{
	fy_generic v = fy_get(doc, "api");

	if (fy_equal(v, "chat-completions") || fy_equal(v, "chat"))
		return FYAI_API_CHAT_COMPLETIONS;
	if (fy_equal(v, "responses"))
		return FYAI_API_RESPONSES;
	if (fy_equal(v, "messages"))
		return FYAI_API_MESSAGES;
	return dflt;
}

/*
 * On a model change that lands on a catalogue entry, clear `api`/`api_url`
 * and re-derive them from the catalogue (preferring the grammar @doc already
 * asked for, falling back to whatever the provider actually offers) - the
 * same explicit values `fyai init` would produce importing a fresh sample.
 * A no-op when the model resolved to the same catalogue entry as before.
 */
static fy_generic config_doc_sync_derived_api(struct fy_generic_builder *gb,
					      fy_generic cat_prov,
					      fy_generic doc,
					      fy_generic *changesp)
{
	fy_generic cat_ep, before, after, new_api, new_api_url;
	enum fyai_api_mode mode;
	int i;

	/* Preference must be read before clearing, or it is always lost. */
	mode = config_doc_api_mode(doc, FYAI_API_RESPONSES);
	doc = fy_delete_at_pathstr(gb, doc, "api");
	doc = fy_delete_at_pathstr(gb, doc, "api_url");

	cat_ep = fyai_catalog_endpoint(cat_prov, mode);
	for (i = 0; i < 3 && fy_generic_is_invalid(cat_ep); i++) {
		cat_ep = fyai_catalog_endpoint(cat_prov, (enum fyai_api_mode)i);
		if (fy_generic_is_valid(cat_ep))
			mode = (enum fyai_api_mode)i;
	}
	if (fy_generic_is_invalid(cat_ep))
		return doc;

	new_api = fy_value(fyai_api_to_string(mode));
	new_api_url = fy_value(fy_sprintfa("%s%s",
					   fy_get(cat_prov, "root_url", ""),
					   fy_get(cat_ep, "endpoint", "")));

	before = fy_invalid;
	doc = fy_set_at_pathstr(gb, doc, "api", new_api);
	if (changesp) {
		after = fy_get(doc, "api");
		*changesp = config_change_add(gb, *changesp, "api",
					      "changed", before, after);
	}

	before = fy_invalid;
	doc = fy_set_at_pathstr(gb, doc, "api_url", new_api_url);
	if (changesp) {
		after = fy_get(doc, "api_url");
		*changesp = config_change_add(gb, *changesp, "api_url",
					      "changed", before, after);
	}
	return doc;
}

/*
 * Re-derive the read-only `catalog:` block on @doc from @catalog: the full
 * models[] entry for the configured model, plus `canonical_provider` (the
 * default, unprefixed provider offering it). Keyed off `model`, honouring a
 * `provider/` prefix only when it names a catalogue provider (same rule as
 * fyai_config_resolve_model). Removed entirely when the model is unknown to
 * @catalog, so switching to an uncatalogued model drops stale properties.
 * A model change onto a known catalogue entry also re-derives `api`/
 * `api_url` (config_doc_sync_derived_api).
 */
static fy_generic catalog_sync_config_doc(struct fy_generic_builder *gb,
					  fy_generic catalog, fy_generic doc,
					  fy_generic *changesp)
{
	fy_generic model_v, cat_model, cat_prov, pinned, block, before, after;
	const char *model, *bare, *slash;
	char *pfx;

	model_v = fy_get(doc, "model");
	model = fy_castp(&model_v, "");
	bare = model;
	pinned = fy_invalid;
	slash = (model && *model) ? strchr(model, '/') : NULL;
	if (slash) {
		pfx = strndup(model, slash - model);
		if (pfx) {
			pinned = fyai_catalog_provider(catalog, pfx);
			if (fy_generic_is_valid(pinned))
				bare = slash + 1;
			free(pfx);
		}
	}

	cat_model = fyai_catalog_resolved_model(catalog, bare);
	before = fy_get(doc, "catalog");
	if (fy_generic_is_invalid(cat_model)) {
		if (fy_generic_is_invalid(before))
			return doc;
		doc = fy_delete_at_pathstr(gb, doc, "catalog");
		if (changesp)
			*changesp = config_change_add(gb, *changesp, "catalog",
						      "removed", before, fy_invalid);
		return doc;
	}

	cat_prov = fyai_catalog_provider_for_model(catalog, bare, NULL);
	block = fy_assoc(gb, cat_model, "canonical_provider",
			 fy_generic_is_valid(cat_prov) ?
				 fy_get(cat_prov, "name", "") : "");
	if (fy_equal(before, block))
		return doc;
	doc = fy_set_at_pathstr(gb, doc, "catalog", block);
	if (changesp) {
		after = fy_get(doc, "catalog");
		*changesp = config_change_add(gb, *changesp, "catalog",
					      fy_generic_is_invalid(before) ?
					      "added" : "changed", before, after);
	}
	if (fy_generic_is_valid(cat_prov))
		doc = config_doc_sync_derived_api(gb, cat_prov, doc, changesp);
	return doc;
}

fy_generic fyai_config_sync_catalog(struct fy_generic_builder *gb,
				    fy_generic catalog, fy_generic config_doc)
{
	return catalog_sync_config_doc(gb, catalog, config_doc, NULL);
}

static fy_generic config_doc_sanitize(struct fyai_cfg *cfg, fy_generic doc,
				      fy_generic *changesp)
{
	fy_generic changes, before, after;
	fy_generic api;
	bool builtin_shell, tools, logprobs;
	long long top_logprobs;

	changes = fy_seq_empty;

	api = fy_get(doc, "api");
	if (fy_equal(api, "chat")) {
		before = fy_get(doc, "api");
		doc = fy_set_at_pathstr(cfg->gb, doc, "api",
					fy_value("chat-completions"));
		after = fy_get(doc, "api");
		changes = config_change_add(cfg->gb, changes, "api", "changed",
					    before, after);
		api = after;
	}

	top_logprobs = fy_get(doc, "top_logprobs", -1LL);
	logprobs = fy_get(doc, "logprobs", false);
	if (top_logprobs >= 0 && !logprobs) {
		before = fy_get(doc, "logprobs");
		doc = fy_set_at_pathstr(cfg->gb, doc, "logprobs", true);
		after = fy_get(doc, "logprobs");
		changes = config_change_add(cfg->gb, changes, "logprobs",
					    fy_generic_is_invalid(before) ?
					    "added" : "changed",
					    before, after);
	}

	builtin_shell = fy_get(doc, "builtin_shell", false);
	tools = fy_get(doc, "tools", false);
	if (fy_equal(api, "messages") && builtin_shell) {
		before = fy_get(doc, "builtin_shell");
		doc = fy_set_at_pathstr(cfg->gb, doc, "builtin_shell", false);
		after = fy_get(doc, "builtin_shell");
		changes = config_change_add(cfg->gb, changes, "builtin_shell",
					    "changed", before, after);
		if (!tools) {
			before = fy_get(doc, "tools");
			doc = fy_set_at_pathstr(cfg->gb, doc, "tools", true);
			after = fy_get(doc, "tools");
			changes = config_change_add(cfg->gb, changes, "tools",
						    fy_generic_is_invalid(before) ?
						    "added" : "changed",
						    before, after);
		}
	}

	doc = catalog_sync_config_doc(cfg->gb,
				      fyai_catalog_effective(cfg->catalog, cfg->gb),
				      doc, &changes);

	*changesp = changes;
	return doc;
}

/*
 * Mirror @key from @root into @config_doc: set when present in @root, delete
 * when absent (and only if @config_doc actually carries it, since deleting a
 * missing key is an error).
 */
static fy_generic config_doc_mirror_key(struct fy_generic_builder *gb,
					fy_generic config_doc, fy_generic root,
					const char *key)
{
	fy_generic v;

	v = fy_get_at_pathstr(gb, root, key);
	if (fy_generic_is_valid(v))
		return fy_set_at_pathstr(gb,
				fy_generic_is_valid(config_doc) ?
					config_doc : fy_map_empty,
				key, v);
	if (fy_generic_is_invalid(config_doc) ||
	    fy_generic_is_invalid(fy_get_at_pathstr(gb, config_doc, key)))
		return config_doc;
	return fy_delete_at_pathstr(gb, config_doc, key);
}

static fy_generic config_validate_report_shallow(struct fyai_cfg *cfg,
						 fy_generic doc,
						 const char *origin)
{
	fy_generic problems, v, schema, schema_report, schema_problems, p;

	problems = fy_seq_empty;
	if (!fy_generic_is_mapping(doc))
		problems = config_problem_add(cfg->gb, problems,
				"%s is not a YAML mapping", origin);
	if (fyai_config_has_raw_secret(doc))
		problems = config_problem_add(cfg->gb, problems,
				"%s carries a raw api_key; use { type: env|secret, value: NAME }",
				origin);
	if (!fy_generic_is_mapping(doc))
		goto out;
	v = fy_get(doc, "api_key");
	if (fy_generic_is_mapping(v) &&
	    (fy_equal(fy_get(v, "type"), "env") ||
	     fy_equal(fy_get(v, "type"), "secret")) &&
	    !*fy_get(v, "value", ""))
		problems = config_problem_add(cfg->gb, problems,
				"%s: api_key.value is required for type %s", origin,
				fy_get(v, "type", ""));

	/*
	 * Structural/type/enum check against the vendored JSON Schema
	 * (data/config.schema.yaml), ahead of the semantic checks below -
	 * catches shape errors (wrong type, bad enum value, missing required
	 * api_key.value, ...) with one rule set shared with `config
	 * validate`/`config schema` instead of duplicating them by hand.
	 */
	schema = fyai_config_schema(cfg->gb);
	schema_report = fyai_schema_validate(cfg->gb, schema, doc);
	if (!fyai_schema_valid(schema_report)) {
		schema_problems = fy_get(schema_report, "problems", fy_seq_empty);
		fy_foreach(p, schema_problems)
			problems = config_problem_add(cfg->gb, problems,
					"%s: schema: %s", origin,
					fy_castp(&p, ""));
	}

	v = fy_get(doc, "api");
	if (!fy_generic_is_invalid(v) &&
	    !config_contains(fy_sequence("responses", "messages", "chat",
					 "chat-completions"), v))
		problems = config_problem_add(cfg->gb, problems,
				"invalid api '%s' (responses|chat-completions|messages)",
				fy_cast(fy_convert(v, FYGT_STRING), ""));

	v = fy_get(doc, "reasoning");
	if (!fy_generic_is_invalid(v) && !fy_generic_is_mapping(v))
		problems = config_problem_add(cfg->gb, problems,
				"reasoning must be a mapping");
	if (fy_generic_is_mapping(v)) {
		fy_generic e, s;

		e = fy_get(v, "effort");
		if (!fy_generic_is_invalid(e) &&
		    !config_contains(fy_sequence("minimal", "low",
						 "medium", "high"), e))
			problems = config_problem_add(cfg->gb, problems,
				"invalid reasoning.effort '%s' (minimal|low|medium|high)",
				fy_cast(fy_convert(e, FYGT_STRING), ""));
		s = fy_get(v, "summary");
		if (!fy_generic_is_invalid(s) &&
		    !config_contains(fy_sequence("auto", "concise",
						 "detailed"), s))
			problems = config_problem_add(cfg->gb, problems,
				"invalid reasoning.summary '%s' (auto|concise|detailed)",
				fy_cast(fy_convert(s, FYGT_STRING), ""));
	}

	v = fy_get(doc, "display");
	if (!fy_generic_is_invalid(v) && !fy_generic_is_mapping(v))
		problems = config_problem_add(cfg->gb, problems,
				"display must be a mapping");
	if (fy_generic_is_mapping(v)) {
		fy_generic mode, color, theme, mdtheme, tborder;

		mode = fy_get(v, "markdown_mode");
		if (!fy_generic_is_invalid(mode) &&
		    !config_contains(fy_sequence("oneshot", "line",
						 "stream"), mode))
			problems = config_problem_add(cfg->gb, problems,
				"invalid display.markdown_mode '%s' (oneshot|line|stream)",
				fy_cast(fy_convert(mode, FYGT_STRING), ""));
		color = fy_get(v, "color");
		if (!fy_generic_is_invalid(color) &&
		    !config_contains(fy_sequence("auto", "off",
						 "on"), color))
			problems = config_problem_add(cfg->gb, problems,
				"invalid display.color '%s' (auto|off|on)",
				fy_cast(fy_convert(color, FYGT_STRING), ""));
		theme = fy_get(v, "theme");
		if (!fy_generic_is_invalid(theme) &&
		    !config_contains(fy_sequence("auto", "dark",
						 "light"), theme))
			problems = config_problem_add(cfg->gb, problems,
				"invalid display.theme '%s' (auto|dark|light)",
				fy_cast(fy_convert(theme, FYGT_STRING), ""));
		mdtheme = fy_get(v, "markdown_theme");
		if (!fy_generic_is_invalid(mdtheme) &&
		    !markdown_theme_valid(fy_cast(fy_convert(mdtheme,
							     FYGT_STRING), "")))
			problems = config_problem_add(cfg->gb, problems,
				"invalid display.markdown_theme '%s' (see 'fyai config' / libfymd4c themes)",
				fy_cast(fy_convert(mdtheme, FYGT_STRING), ""));
		tborder = fy_get(v, "table_border");
		if (!fy_generic_is_invalid(tborder) &&
		    !config_contains(fy_sequence("theme", "grid", "none"),
				     tborder))
			problems = config_problem_add(cfg->gb, problems,
				"invalid display.table_border '%s' (theme|grid|none)",
				fy_cast(fy_convert(tborder, FYGT_STRING), ""));
	}

out:
	return problems;
}

fy_generic fyai_config_validate_report(struct fyai_cfg *cfg, fy_generic doc,
				       const char *origin)
{
	struct fyai_cfg tmp;
	fy_generic problems, changes, sanitized;

	problems = config_validate_report_shallow(cfg, doc, origin);
	if (config_problem_any(problems))
		return fy_gb_mapping(cfg->gb, "result", "failed",
				     "problems", problems);

	memset(&tmp, 0, sizeof(tmp));
	tmp.gb = cfg->gb;
	tmp.catalog = cfg->catalog;
	fyai_config_set_defaults(&tmp);
	tmp.gb = cfg->gb;
	tmp.catalog = cfg->catalog;
	if (apply_config(&tmp, doc))
		problems = config_problem_add(cfg->gb, problems,
				"%s has invalid values", origin);
	else if (config_doc_finalize(&tmp))
		problems = config_problem_add(cfg->gb, problems,
				"%s fails semantic validation after resolution", origin);

	if (config_problem_any(problems))
		return fy_gb_mapping(cfg->gb, "result", "failed",
				     "problems", problems);

	sanitized = config_doc_sanitize(cfg, doc, &changes);
	return fy_gb_mapping(cfg->gb, "result", "ok",
			     "config", sanitized,
			     "changes", changes);
}

int fyai_config_validate_document(struct fyai_cfg *cfg, fy_generic doc,
				       const char *origin)
{
	fy_generic report, problems, problem;

	report = fyai_config_validate_report(cfg, doc, origin);
	if (fy_equal(fy_get(report, "result"), "ok"))
		return 0;
	problems = fy_get(report, "problems", fy_seq_empty);
	fy_foreach(problem, problems)
		fprintf(stderr, "config: %s\n", fy_castp(&problem, ""));
	return -1;
}

static fy_generic embedded_config_schema = fy_invalid;

fy_generic fyai_config_schema(struct fy_generic_builder *gb)
{
	fy_generic_sized_string embedded;

	if (fy_generic_is_valid(embedded_config_schema))
		return embedded_config_schema;
	embedded.data = (const char *)FYAI_EMBEDDED_CONFIG_SCHEMA;
	embedded.size = FYAI_EMBEDDED_CONFIG_SCHEMA_LEN;
	embedded_config_schema = fy_parse(gb, embedded,
					  FYOPPF_DISABLE_DIRECTORY |
					  FYOPPF_MODE_YAML_1_2 |
					  FYOPPF_INPUT_TYPE_STRING, NULL);
	return embedded_config_schema;
}

int fyai_config_validate_schema(struct fyai_cfg *cfg, fy_generic doc,
				const char *origin)
{
	fy_generic schema, report;

	schema = fyai_config_schema(cfg->gb);
	report = fyai_schema_validate(cfg->gb, schema, doc);
	if (fyai_schema_valid(report))
		return 0;
	fprintf(stderr, "%s: schema validation failed\n", origin);
	fyai_schema_report_print(report);
	return -1;
}

static int config_report_commit(fy_generic report, fy_generic *docp)
{
	fy_generic problems, problem;

	if (fy_equal(fy_get(report, "result"), "ok")) {
		if (docp)
			*docp = fy_get(report, "config", *docp);
		return 0;
	}
	problems = fy_get(report, "problems", fy_seq_empty);
	fy_foreach(problem, problems)
		fprintf(stderr, "config: %s\n", fy_castp(&problem, ""));
	return -1;
}

int fyai_config_delete(struct fyai_ctx *ctx, const char *key)
{
	struct fy_generic_builder *gb = ctx->gb;
	fy_generic report;
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
	report = fyai_config_validate_report(ctx->cfg, root, "config");
	if (config_report_commit(report, &root))
		return -1;
	ctx->cfg->config_doc = config_doc_mirror_key(gb, ctx->cfg->config_doc,
						     root, key);
	ctx->cfg->config_doc = config_doc_mirror_key(gb, ctx->cfg->config_doc,
						     root, "catalog");
	return fyai_publish_root(ctx, root, fy_invalid, fy_invalid);
}

int fyai_config_import(struct fyai_ctx *ctx, const char *path)
{
	fy_generic report;
	fy_generic doc;

	if (!ctx->gb) {
		fprintf(stderr, "config: no arena; run fyai init\n");
		return -1;
	}
	doc = fy_parse_file(ctx->gb,
			    FYOPPF_DISABLE_DIRECTORY | FYOPPF_MODE_YAML_1_2,
			    path);
	report = fyai_config_validate_report(ctx->cfg, doc, path);
	if (config_report_commit(report, &doc))
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
	fy_generic report;
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
	report = fyai_config_validate_report(ctx->cfg, doc, "edited config");
	if (config_report_commit(report, &doc)) {
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

int fyai_config_rederive(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;

	if (!cfg || !cfg->gb)
		return -1;

	/*
	 * The arena is authoritative after a mutation; rebuild the merged doc
	 * from it (edit/import replace the whole document, set/delete already
	 * mirrored it) and re-run the single apply pass so the derived cache
	 * matches what a fresh process would compute.
	 */
	if (fy_generic_is_valid(ctx->arena_config))
		cfg->config_doc = ctx->arena_config;
	if (apply_config(cfg, cfg->config_doc))
		return -1;

	/* Re-resolve theme=auto to a concrete value before reloading style. */
	if (cfg->theme && !strcmp(cfg->theme, "auto"))
		cfg->theme = (cfg->markdown &&
			      markdown_color_enabled(cfg->color)) ?
				terminal_detect_theme() : "dark";
	if (cfg->markdown)
		fyai_markdown_load_style(cfg);

	return fyai_config_resolve_model(cfg);
}

void fyai_config_set_defaults(struct fyai_cfg *cfg)
{
	cfg->api_key_auto = true;
	cfg->auth_mode = FYAI_AUTH_AUTO;
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
	cfg->thinking = DEFAULT_THINKING;
	cfg->wire_logging = false;
	cfg->stream_logging = false;
	cfg->conversation_logging = false;
	cfg->whitewash_api_keys = true;
	/* This is an opt-in because provider-side response retention is lossy. */
	cfg->response_chain = false;
	cfg->markdown_mode = DEFAULT_MARKDOWN_MODE;
	cfg->color = DEFAULT_COLOR;
	cfg->theme = DEFAULT_THEME;
	cfg->code_theme = NULL;		/* NULL => the theme's own code.theme */
	cfg->markdown_theme = NULL;	/* NULL => the libfymd4c default theme */
	cfg->turn_separator = DEFAULT_TURN_SEPARATOR;
	cfg->tool_separator = DEFAULT_TOOL_SEPARATOR;
	cfg->section_separator = DEFAULT_SECTION_SEPARATOR;
	cfg->prompt_marker = "";	/* empty => built-in prompt marker */
	cfg->prompt_top = "";		/* empty => blank styled top row */
	cfg->prompt_bottom = "";	/* empty => DEFAULT_PROMPT_BOTTOM banner */
	cfg->table_border = 0;		/* 0 => follow the theme's table.border */
	cfg->catalog = fy_invalid;
	cfg->config_doc = fy_invalid;
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
	OPT_SANDBOX = 256,
	OPT_NEW,
	OPT_ANSWER,
	OPT_COLOR,
	OPT_THEME,
	OPT_CODE_THEME,
	OPT_SET,
	OPT_GET,
	OPT_DELETE,
	OPT_TRANSIENT,
};

static const struct option long_options[] = {
	{ "help", no_argument, NULL, 'h' },
	{ "config", required_argument, NULL, 'C' },
	{ "env", required_argument, NULL, 'e' },
	{ "model", required_argument, NULL, 'm' },
	{ "api-key", required_argument, NULL, 'k' },
	{ "url", required_argument, NULL, 'u' },
	{ "tools", no_argument, NULL, 't' },
	{ "sandbox", no_argument, NULL, OPT_SANDBOX },
	{ "color", required_argument, NULL, OPT_COLOR },
	{ "theme", required_argument, NULL, OPT_THEME },
	{ "code-theme", required_argument, NULL, OPT_CODE_THEME },
	{ "new", no_argument, NULL, OPT_NEW },
	{ "interactive", no_argument, NULL, 'i' },
	{ "debug", no_argument, NULL, 'd' },
	{ "cache-info", no_argument, NULL, 'c' },
	{ "answer", required_argument, NULL, OPT_ANSWER },
	{ "set", required_argument, NULL, OPT_SET },
	{ "get", required_argument, NULL, OPT_GET },
	{ "delete", required_argument, NULL, OPT_DELETE },
	{ "transient", no_argument, NULL, OPT_TRANSIENT },
	{ "ephemeral", no_argument, NULL, OPT_TRANSIENT },
	{ NULL, 0, NULL, 0 },
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
	const char *secret_name;
	char *secret = NULL;
	size_t secret_len = 0;
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
	 * Look the model up in the effective catalogue (pinned to the
	 * prefixed provider when given, else the canonical provider) so the
	 * provider name - and, unless an endpoint was already supplied, the
	 * URL, API grammar and wire model id - come from its offering.
	 * Provider resolution runs regardless of whether api_url is already
	 * set (e.g. a persisted override): the two are independent, and
	 * skipping it left cfg->provider on the "unknown model" default
	 * below whenever an api_url happened to be preset. Unknown models
	 * fall through to the static defaults below.
	 */
	if (fy_generic_is_valid(pinned_prov)) {
		cat_prov = pinned_prov;
		fyai_catalog_offering(cat_prov, cfg->model, &cat_offer);
	} else {
		cat_prov = fyai_catalog_provider_for_model(catalog,
					cfg->model, &cat_offer);
	}
	if (fy_generic_is_valid(cat_prov))
		cfg->provider = fy_gb_intern_string(cfg->gb,
					fy_get(cat_prov, "name", ""));
	pmid = fy_get(cat_offer, "provider_model_id", "");
	if (*pmid)
		cfg->model = fy_gb_intern_string(cfg->gb, pmid);

	if (!cfg->api_url || !*cfg->api_url) {
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
		if (fy_generic_is_valid(cat_ep))
			cfg->api_url = fy_gb_intern_string(cfg->gb,
					fy_sprintfa("%s%s",
						fy_get(cat_prov, "root_url", ""),
						fy_get(cat_ep, "endpoint", "")));
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
	if (cfg->api_key_auto && (!cfg->api_key || !*cfg->api_key) &&
	    cfg->provider && *cfg->provider) {
		env = provider_env_key(cfg->gb, cfg->provider);
		if (env) {
			val = getenv(env);
			if (val && *val)
				cfg->api_key = fy_gb_intern_string(cfg->gb, val);
		}
		secret_name = fy_sprintfa("fyai:api-key/%s", cfg->provider);

		if ((!cfg->api_key || !*cfg->api_key) &&
		    fyai_secret_kernel_get(secret_name, &secret, &secret_len) == FYAI_SECRET_OK) {
			cfg->api_key = fy_gb_intern_string(cfg->gb, secret);
			fyai_secret_clear_and_free(&secret, &secret_len);
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

static int config_queue_op(struct fyai_cfg *cfg, char op, const char *key,
			   const char *value, bool persistent, bool command)
{
	struct fyai_config_op *co;

	if (cfg->config_op_count >= ARRAY_SIZE(cfg->config_ops)) {
		fprintf(stderr, "too many config operations, max %zu\n",
			ARRAY_SIZE(cfg->config_ops));
		return -1;
	}
	co = &cfg->config_ops[cfg->config_op_count++];
	co->op = op;
	co->key = fy_gb_intern_string(cfg->gb, key);
	co->value = value ? fy_gb_intern_string(cfg->gb, value) : NULL;
	co->persistent = persistent;
	co->command = command;
	return 0;
}

static int config_queue_set_literal(struct fyai_cfg *cfg, const char *key,
				    const char *value, bool persistent,
				    bool command)
{
	return config_queue_op(cfg, 's', key, value, persistent, command);
}

static int config_queue_set_quoted(struct fyai_cfg *cfg, const char *key,
				   const char *value, bool persistent,
				   bool command)
{
	char *buf, *p;
	const char *s;
	size_t len, extra;
	int rc;

	extra = 2;
	for (s = value; *s; s++) {
		extra++;
		if (*s == '\'')
			extra++;
	}
	buf = malloc(extra + 1);
	if (!buf)
		return -1;
	p = buf;
	*p++ = '\'';
	for (s = value; *s; s++) {
		*p++ = *s;
		if (*s == '\'')
			*p++ = '\'';
	}
	*p++ = '\'';
	*p = '\0';
	rc = config_queue_set_literal(cfg, key, buf, persistent, command);
	len = strlen(buf);
	memset(buf, 0, len);
	free(buf);
	return rc;
}

static bool config_has_command_ops(struct fyai_cfg *cfg)
{
	size_t i;

	for (i = 0; i < cfg->config_op_count; i++)
		if (cfg->config_ops[i].command)
			return true;
	return false;
}

/*
 * Fold every pending --set into @cfg as the highest-precedence layer, so this
 * invocation sees the edited values. Builds one transient document by replaying
 * every queued set/delete in order, then overlays it via apply_config.
 */
static int apply_config_set_ops(struct fyai_cfg *cfg)
{
	struct fyai_config_op *co;
	fy_generic doc, v;
	size_t i;

	if (!cfg->config_op_count)
		return 0;
	doc = fy_generic_is_valid(cfg->config_doc) ?
	      cfg->config_doc : fy_map_empty;
	for (i = 0; i < cfg->config_op_count; i++) {
		co = &cfg->config_ops[i];
		switch (co->op) {
		case 's':
			v = config_parse_value(cfg->gb, co->value);
			if (fy_generic_is_invalid(v)) {
				fprintf(stderr,
					"config override %s: cannot parse value '%s'\n",
					co->key, co->value);
				return -1;
			}
			doc = fy_set_at_pathstr(cfg->gb, doc, co->key, v);
			if (fy_generic_is_invalid(doc)) {
				fprintf(stderr, "config override %s: failed\n",
					co->key);
				return -1;
			}
			break;
		case 'd':
			doc = fy_delete_at_pathstr(cfg->gb, doc, co->key);
			if (fy_generic_is_invalid(doc)) {
				fprintf(stderr, "config override %s: delete failed\n",
					co->key);
				return -1;
			}
			break;
		default:
			break;
		}
	}
	/* CLI --set/-m overrides never touch the arena, but `config effective`
	 * (and anything else reading cfg->config_doc this run) should still
	 * see the catalog: block for whatever model they landed on. */
	doc = fyai_config_sync_catalog(cfg->gb,
				       fyai_catalog_effective(cfg->catalog, cfg->gb),
				       doc);
	cfg->config_doc = doc;
	return apply_config(cfg, doc);
}

int fyai_config_setup(struct fyai_cfg *cfg, int argc, char *argv[])
{
	struct fy_generic_builder_cfg gb_cfg;
	const char *cli_config, *cli_env, *cli_api_key;
	int opt, rc, arg_index;
	char *def_arena_dir = NULL;
	const char *verb = NULL;
	bool stdin_prompt;
	char *prompt = NULL;
	int ret = -1;
	char tmp_prompt[16];
	char *tmp_argv[2];

	if (!cfg)
		return -1;

	memset(cfg, 0, sizeof(*cfg));

	memset(&gb_cfg, 0, sizeof(gb_cfg));
	gb_cfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	cfg->gb = fy_generic_builder_create(&gb_cfg);
	if (!cfg->gb)
		goto err_out;

	fyai_config_set_defaults(cfg);

	cli_config = NULL;
	cli_env = NULL;
	cli_api_key = NULL;

	optind = 0;
	optarg = NULL;

	/* '+' stops parsing at the first non-option (the verb or prompt). */
	while ((opt = getopt_long(argc, argv, "+hC:e:m:k:u:tidc",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			fyai_usage(stdout, "fyai", cfg->color);
			ret = 1;
			goto out;
		case 'C':
			cli_config = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case 'e':
			cli_env = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case 'm':
			if (config_queue_set_quoted(cfg, "model", optarg,
						    false, false))
				goto err_out;
			break;
		case 'k':
			cli_api_key = fy_gb_intern_string(cfg->gb, optarg);
			break;
		case 'u':
			if (config_queue_set_quoted(cfg, "api_url", optarg,
						    false, false))
				goto err_out;
			break;
		case 't':
			if (config_queue_set_literal(cfg, "tools", "true",
						     false, false))
				goto err_out;
			break;
		case OPT_SANDBOX:
			if (config_queue_set_literal(cfg, "sandbox", "true",
						     false, false))
				goto err_out;
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
		case 'c':
			if (config_queue_set_literal(cfg, "display/cache_info",
						     "true", false, false))
				goto err_out;
			break;
		case OPT_COLOR:
			if (config_queue_set_quoted(cfg, "display/color", optarg,
						    false, false))
				goto err_out;
			break;
		case OPT_THEME:
			if (config_queue_set_quoted(cfg, "display/theme", optarg,
						    false, false))
				goto err_out;
			break;
		case OPT_CODE_THEME:
			if (config_queue_set_quoted(cfg, "display/code_theme",
						    optarg, false, false))
				goto err_out;
			break;
		case OPT_ANSWER:
			if (cfg->answer_count >= ARRAY_SIZE(cfg->answers)) {
				fprintf(stderr, "too many answers, max %zu\n",
					ARRAY_SIZE(cfg->answers));
				goto err_out;
			}
			cfg->answers[cfg->answer_count++] =
				fy_gb_intern_string(cfg->gb, optarg);
			break;
		case OPT_SET:
			{
				char *eq, *k;

				eq = strchr(optarg, '=');
				if (eq) {
					k = strndup(optarg, eq - optarg);
					if (!k)
						goto err_out;
					rc = config_queue_op(cfg, 's', k, eq + 1,
							    true, true);
					free(k);
				} else {
					if (optind >= argc) {
						fprintf(stderr,
							"--set %s: missing value\n",
							optarg);
						goto err_out;
					}
					rc = config_queue_op(cfg, 's', optarg,
							    argv[optind++],
							    true, true);
				}
				if (rc)
					goto err_out;
			}
			break;
		case OPT_GET:
			if (config_queue_op(cfg, 'g', optarg, NULL, false, true))
				goto err_out;
			break;
		case OPT_DELETE:
			if (config_queue_op(cfg, 'd', optarg, NULL, true, true))
				goto err_out;
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
	arg_index = optind;

	rc = fyai_config_load(cfg, cli_config, cli_env);
	if (rc)
		goto err_out;

	/*
	 * Fold pending --set edits into this run before model/theme resolution,
	 * so `--set model=X` (or display/theme, etc.) takes effect immediately.
	 * They also persist at storage-open time (see fyai_apply_config_ops).
	 */
	if (apply_config_set_ops(cfg))
		goto err_out;
	/* Command-line secrets have absolute precedence over configuration
	 * intent, including a pending --set api_key operation. */
	if (cli_api_key) {
		cfg->api_key = cli_api_key;
		cfg->api_key_explicit = true;
		cfg->api_key_auto = false;
	}

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
	verb = arg_index < argc && fyai_is_verb(argv[arg_index]) ? argv[arg_index] : NULL;

	if (!verb && arg_index >= argc && !cfg->interactive &&
	    config_has_command_ops(cfg)) {
		/*
		 * A bare --set/--get/--delete run (no verb, no prompt, and not
		 * -i) is a config-only invocation: dispatch the config verb as
		 * a no-op so storage opens without requiring an API key; the
		 * global ops run in fyai_run. An explicit -i still wants the
		 * interactive prompt loop even with pending --set ops queued -
		 * those still run in fyai_run either way.
		 */
		cfg->cmd.id = FYAIVID_CONFIG;
		cfg->cmd.args.config.type = FYAICT_NOOP;
		ret = 0;
	} else if (verb) {
		cfg->cmd.id = fyai_get_verb_id(verb);
		argv += arg_index;
		argc -= arg_index;
	} else {
		stdin_prompt = (!cfg->interactive && arg_index >= argc &&
				!terminal_is_tty(STDIN_FILENO)) ||
			       (arg_index < argc && argc - arg_index == 1 &&
				!strcmp(argv[arg_index], "-"));

		/* No prompt and a terminal on stdin: drop into interactive mode. */
		if (arg_index >= argc && !stdin_prompt)
			cfg->interactive = true;

		prompt = NULL;
		if (stdin_prompt) {
			prompt = read_all_stdin();
			if (!prompt) {
				fprintf(stderr, "failed to read prompt from stdin\n");
				goto err_out;
			}
		} else if (arg_index < argc) {
			prompt = join_args(argc - arg_index, argv + arg_index);
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
	fyai_config_cleanup(cfg);
	return ret;

err_out:
	ret = -1;
	goto out;
}
