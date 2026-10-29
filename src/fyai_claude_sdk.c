/*
 * fyai_claude_sdk.c - the Claude Agent SDK protocol on standard I/O
 *
 * SPDX-License-Identifier: MIT
 *
 * A Claude Agent SDK runs its CLI as a child and exchanges JSON lines with
 * it: user messages and control requests on standard input, conversation
 * messages, results and control responses on standard output. This front
 * end serves that protocol for one session, which is a branch sdk/<id>.
 * The process lives while the SDK keeps standard input open; each user
 * message runs one turn, which is published before its result is sent.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fnmatch.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/rand.h>

#include "fyai.h"
#include "fyai_auth_util.h"
#include "fyai_branch.h"
#include "fyai_claude_sdk.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_config.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_media.h"
#include "fyai_output.h"
#include "fyai_grammar.h"
#include "fyai_provider.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_tool_registry.h"
#include "fyai_tool_spec.h"
#include "fyai_tools.h"
#include "fyai_turn.h"
#include "fyai_xfer.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

#define SDK_BRANCH_PREFIX	"sdk/"
#define SDK_ID_MAX		64

/* An image block of a user message: the bytes, decoded. */
struct sdk_image {
	struct sdk_image *next;
	char *media_type;
	char *name;
	unsigned char *data;
	size_t len;
};

/* One user message that waits for its turn. */
struct sdk_input {
	struct sdk_input *next;
	char *text;
	struct sdk_image *images;	/* the image blocks, in order */
	char *origin_json;		/* the origin that the SDK stamped */
	bool composed;			/* the text goes as written */
};

/* A permission rule: a tool, and the text in its parentheses or NULL. */
struct sdk_rule {
	char *tool;
	char *spec;
};

struct sdk_rules {
	struct sdk_rule *v;
	size_t n;
};

/* One callback of a hook of the SDK, for one event and matcher. */
struct sdk_hook {
	char *event;
	char *callback_id;
	bool every;			/* no matcher: every tool */
	regex_t re;
};

struct sdk_server;
struct sdk_pending;

typedef void (*sdk_done_fn)(struct sdk_server *srv, struct sdk_pending *p,
			    fy_generic response, const char *error);

/*
 * A control request to the SDK that waits for its response. It names what
 * the response completes: a decision on a tool call, a wait of the front
 * end, or a message of a tunnelled MCP server.
 */
struct sdk_pending {
	struct sdk_pending *next;
	char id[48];
	sdk_done_fn done;
	struct sdk_decision *decision;
	struct sdk_wait *wait;
	struct sdk_tunnel *tunnel;
	fy_generic rpc_id;		/* the JSON-RPC ID of a tunnelled request */
	char *rpc_id_json;
};

/*
 * What the hooks and the permission policy of the SDK make of one call of a
 * round of the tool gate. It keeps text, not generics: an answer can arrive
 * after the turn and its transient storage have ended.
 */
struct sdk_decision {
	struct sdk_decision *next;
	uint64_t gate_id;
	size_t index;
	bool after;
	int waiting;			/* responses that are outstanding */
	bool answered;			/* the gate has its answer */
	char *claude_name;		/* the name that the SDK sees */
	char *fyai_name;
	char *args_json;		/* the arguments of the call */
	char *input_json;		/* the input that the SDK sees */
	char *tool_use_id;
	char *result_json;		/* after: the result of the call */
	bool ok;
	enum { SDK_NONE, SDK_ALLOW, SDK_DENY, SDK_ASK } verdict;
	char *reason;
	char *updated_json;		/* an input that replaces the input */
	char *context;			/* after: text for the model */
	bool stop;
	char *stop_reason;
};

/* The merged outputs of the hooks of one event that the front end waits for. */
struct sdk_wait {
	int waiting;
	bool block;
	bool stop;
	char *reason;
	char *context;
	char *stop_reason;
};

/* An MCP server of the SDK, reached through mcp_message requests. */
struct sdk_tunnel {
	struct sdk_tunnel *next;
	struct sdk_server *srv;
	char *name;
	struct jsonrpc_conn *conn;
};

struct sdk_server {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	struct sdk_input *head;
	struct sdk_input **tail;
	char *session_id;		/* the branch name without its prefix */
	const char *append;		/* --append-system-prompt */
	const char *permission_mode;
	bool initialized;
	bool mcp_starting;		/* the servers of --mcp-config connect */
	char *init_id;			/* an initialize that waits for them */
	bool busy;
	bool failed;			/* a frame could not be queued */
	bool last_error;		/* the last result is an error */
	/* What the session asks of the SDK. */
	bool can_use_tool;		/* the SDK decides on a tool call */
	struct sdk_rules allow;
	struct sdk_rules deny;
	struct sdk_hook *hooks;
	size_t nhooks;
	struct sdk_pending *pending;
	struct sdk_decision *decisions;
	struct sdk_tunnel *tunnels;
	unsigned long long requests;
	fy_generic agents;		/* the names of the agents of the SDK */
	bool partial;			/* stream_event frames of the text */
	bool forward_text;		/* the text of sub-agents too */
	bool stream_open;		/* a streamed message is open */
	/* State of the running turn. */
	long long num_turns;
	char *result_text;		/* the text of the last answer */
	fy_generic denials;		/* the calls it denied, transient */
	const char *model;		/* the model the session started with */
};

/*
 * The fyai tools of a Claude Code tool name. A name of no entry names a fyai
 * tool or an MCP tool itself, or nothing.
 */
static const struct {
	const char *claude;
	const char *fyai[5];
} sdk_tool_map[] = {
	{ "Bash",		{ "exec_command", "shell", "shell_input",
				  "shell_output", "shell_close" } },
	{ "BashOutput",		{ "shell_output" } },
	{ "KillShell",		{ "shell_close" } },
	{ "KillBash",		{ "shell_close" } },
	{ "Read",		{ "read_file" } },
	{ "Write",		{ "write_file" } },
	{ "Edit",		{ "apply_patch" } },
	{ "MultiEdit",		{ "apply_patch" } },
	{ "NotebookEdit",	{ "apply_patch" } },
	{ "TodoWrite",		{ "todo_write" } },
	{ "Task",		{ "agent", "agent_message", "agent_input" } },
	{ "StructuredOutput",	{ "structured_output" } },
	{ "WebSearch",		{ "web_search" } },
	{ "Agent",		{ "agent", "agent_message", "agent_input" } },
	{ "Skill",		{ "skill" } },
	{ "Monitor",		{ "monitor" } },
	{ "AskUserQuestion",	{ "ask_user" } },
};

/* The MCP tools stay with any base set, as the MCP servers do. */
static bool sdk_tool_is_mcp(const char *name)
{
	return !strcmp(name, "tool_search") || !strcmp(name, "mcp_resource");
}

bool fyai_claude_sdk_argv(int argc, char *const argv[])
{
	const char *base;
	int i;

	if (argc < 1 || !argv[0])
		return false;
	/* A wrapper that gives global options names the command itself. */
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "claude-sdk"))
			return false;
	base = strrchr(argv[0], '/');
	base = base ? base + 1 : argv[0];
	if (!strcmp(base, "claude"))
		return true;
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--input-format") ||
		    !strncmp(argv[i], "--input-format=", 15))
			return true;
	return false;
}

char **fyai_claude_sdk_args(int argc, char *argv[])
{
	char **args;
	int i;

	if (!fyai_claude_sdk_argv(argc, argv) ||
	    fyai_claude_sdk_version(argc, argv))
		return NULL;
	args = calloc((size_t)argc + 2, sizeof(*args));
	if (!args)
		return NULL;
	args[0] = argv[0];
	args[1] = (char *)"claude-sdk";
	for (i = 1; i < argc; i++)
		args[i + 1] = argv[i];
	return args;
}

const char *fyai_claude_sdk_version(int argc, char *const argv[])
{
	if (argc != 2 || (strcmp(argv[1], "-v") && strcmp(argv[1], "--version")))
		return NULL;
	return FYAI_CLAUDE_SDK_VERSION " (fyai " VERSION ")";
}

static bool sdk_uuid(char *out, size_t size)
{
	unsigned char b[16];

	if (size < 37 || RAND_bytes(b, sizeof(b)) != 1)
		return false;
	b[6] = (b[6] & 0x0f) | 0x40;
	b[8] = (b[8] & 0x3f) | 0x80;
	snprintf(out, size,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		 "%02x%02x%02x%02x%02x%02x",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
		 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return true;
}

static bool sdk_id_valid(const char *id)
{
	size_t len = strlen(id);
	const char *p;

	if (!len || len > SDK_ID_MAX)
		return false;
	for (p = id; *p; p++)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
		      (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_'))
			return false;
	return true;
}

/* Add the fyai tools of the Claude Code name @name to @set. */
static fy_generic sdk_tools_add(struct fy_generic_builder *gb, fy_generic set,
				const char *name, size_t len)
{
	const struct fyai_tool_def *def;
	size_t i, j;
	char word[128];

	if (!len || len >= sizeof(word))
		return set;
	memcpy(word, name, len);
	word[len] = '\0';
	for (i = 0; i < ARRAY_SIZE(sdk_tool_map); i++) {
		if (strcmp(sdk_tool_map[i].claude, word))
			continue;
		for (j = 0; j < ARRAY_SIZE(sdk_tool_map[i].fyai) &&
			    sdk_tool_map[i].fyai[j]; j++)
			set = fy_append(gb, set, sdk_tool_map[i].fyai[j]);
		return set;
	}
	def = fyai_tool_find(word);
	if (def)
		set = fy_append(gb, set, def->name);
	else if (fyai_mcp_tool_name(word))
		set = fy_append(gb, set, fy_value(gb, word));
	return set;
}

/*
 * The next item of a list separated by commas, as the SDK joins the tools and
 * the rules of an option: a comma inside parentheses is part of a rule. Sets
 * @len and returns the item, or NULL at the end.
 */
static const char *sdk_list_next(const char **pp, size_t *len)
{
	const char *p = *pp, *start;
	int depth = 0;

	while (*p == ' ' || *p == ',')
		p++;
	if (!*p)
		return NULL;
	start = p;
	for (; *p && (depth || *p != ','); p++) {
		if (*p == '(')
			depth++;
		else if (*p == ')' && depth)
			depth--;
	}
	*len = (size_t)(p - start);
	while (*len && start[*len - 1] == ' ')
		(*len)--;
	*pp = p;
	return start;
}

/*
 * The fyai tools of a list of Claude Code names. A rule with parentheses
 * names part of a tool, which the tool gate decides on.
 */
static fy_generic sdk_tools_list(struct fy_generic_builder *gb,
				 const char *list)
{
	fy_generic set = fy_seq_empty;
	const char *p = list, *item;
	size_t len;

	while ((item = sdk_list_next(&p, &len)))
		if (!memchr(item, '(', len))
			set = sdk_tools_add(gb, set, item, len);
	return set;
}

static bool sdk_tools_has(fy_generic set, const char *name)
{
	fy_generic item;

	fy_foreach(item, set)
		if (fy_equal(item, name))
			return true;
	return false;
}

/*
 * The tools that --tools and --disallowedTools leave of the built-in tools and
 * of the MCP tools @mcp. Returns fy_invalid when they restrict nothing, else
 * the names; an empty sequence leaves no tool.
 */
static fy_generic sdk_tools_allowed(struct fy_generic_builder *gb,
				    const char *tools, const char *disallowed,
				    fy_generic mcp)
{
	const struct fyai_tool_def *def;
	fy_generic base, removed, out, item;
	bool all, restricted;
	size_t i;

	all = !tools || !strcmp(tools, "default");
	base = all ? fy_seq_empty : sdk_tools_list(gb, tools);
	removed = disallowed ? sdk_tools_list(gb, disallowed) : fy_seq_empty;
	out = fy_seq_empty;
	restricted = false;
	for (i = 0; (def = fyai_tool_at(i)); i++) {
		if (!all && !sdk_tools_has(base, def->name) &&
		    !sdk_tool_is_mcp(def->name)) {
			restricted = true;
			continue;
		}
		if (sdk_tools_has(removed, def->name)) {
			restricted = true;
			continue;
		}
		if (!sdk_tools_has(out, def->name))
			out = fy_append(gb, out, def->name);
	}
	/* --tools names built-in tools: the MCP tools stay unless removed. */
	fy_foreach(item, mcp) {
		if (sdk_tools_has(removed, fy_castp(&item, ""))) {
			restricted = true;
			continue;
		}
		out = fy_append(gb, out, item);
	}
	return restricted ? out : fy_invalid;
}

/* The options of the SDK that change what the session does, which fyai lacks. */
static const char *const sdk_refused[] = {
	"resume_session_at", "resume_drops_turn", "session_mirror", "plugin_dir",
};

static bool sdk_mode_valid(const char *mode)
{
	return !strcmp(mode, "default") || !strcmp(mode, "acceptEdits") ||
	       !strcmp(mode, "bypassPermissions") || !strcmp(mode, "dontAsk") ||
	       !strcmp(mode, "auto");
}

/*
 * The servers of --mcp-config: a JSON object, or the path of a file that
 * holds one, with the servers under mcpServers. Each server is an MCP server
 * of the SDK (type sdk), a program (type stdio, or a command), or an HTTP
 * server (type http) that needs no header. Returns the mapping of servers
 * in @gb, or fy_invalid with @whyp set.
 */
static fy_generic sdk_mcp_servers(struct fy_generic_builder *gb,
				  const char *text, const char **whyp)
{
	fy_generic doc, servers, name, server, headers;
	const char *type, *nm;
	char *file = NULL;

	*whyp = NULL;
	while (*text == ' ')
		text++;
	if (*text != '{') {
		file = read_text_file(text);
		if (!file) {
			*whyp = fy_sprintfa("cannot read '%s'", text);
			return fy_invalid;
		}
		text = file;
	}
	doc = parse_json_string(gb, text);
	free(file);
	servers = fy_get(doc, "mcpServers", fy_invalid);
	if (!fy_is_mapping(servers)) {
		*whyp = "it has no mcpServers object";
		return fy_invalid;
	}
	fy_foreach_key_value(name, server, servers) {
		nm = fy_castp(&name, "");
		type = fy_get(server, "type", (const char *)NULL);
		if (!type)
			type = fy_is_string(fy_get(server, "command", fy_invalid)) ?
				"stdio" : "";
		headers = fy_get(server, "headers", fy_invalid);
		if (!strcmp(type, "sdk") || !strcmp(type, "stdio"))
			continue;
		if (!strcmp(type, "http") &&
		    fy_is_string(fy_get(server, "url", fy_invalid)) &&
		    (fy_is_invalid(headers) || fy_empty(headers)))
			continue;
		if (!strcmp(type, "http") && !fy_is_invalid(headers) &&
		    !fy_empty(headers))
			*whyp = fy_sprintfa("server '%s' sends headers; fyai "
					    "takes the credentials of a server "
					    "from its configuration", nm);
		else if (!strcmp(type, "http"))
			*whyp = fy_sprintfa("server '%s' has no url", nm);
		else
			*whyp = fy_sprintfa("server '%s' has type '%s', which "
					    "fyai does not support", nm, type);
		return fy_invalid;
	}
	return servers;
}

typedef int (*sdk_setting_fn)(void *data, const char *key, const char *value,
			      bool quoted);

/*
 * The settings that the options of the SDK make for the session, given to
 * @set: a @quoted value is a string, else a YAML flow value. The early hook
 * queues them for the setup; the session puts them in its session layer,
 * where a change of the configuration during the session keeps them.
 */
static int sdk_settings(struct fy_generic_builder *gb, fy_generic args,
			sdk_setting_fn set, void *data)
{
	const char *model, *tools, *disallowed, *rules, *mode;
	fy_generic allowed;
	long long turns;
	int rc = 0;

	/* Only the servers of the SDK: none of the configuration. */
	if (fy_get(args, "strict_mcp_config", false))
		rc = rc ? rc : set(data, "mcp/enabled", "false", false);
	model = fy_get(args, "model", (const char *)NULL);
	if (model && *model)
		rc = rc ? rc : set(data, "model", model, true);
	turns = fy_get(args, "max_turns", 0LL);
	if (turns > 0)
		rc = rc ? rc : set(data, "max_tool_iterations",
				   fy_sprintfa("%lld", turns), false);
	tools = fy_get(args, "tools", (const char *)NULL);
	disallowed = fy_get(args, "disallowed_tools", (const char *)NULL);
	rules = fy_get(args, "allowed_tools", (const char *)NULL);
	mode = fy_get(args, "permission_mode", (const char *)NULL);
	/*
	 * The provider runs web search, so no one is asked for a call: it is
	 * on only when the permission mode or a rule allows it.
	 */
	if ((tools && strcmp(tools, "default") &&
	     !sdk_tools_has(sdk_tools_list(gb, tools), "web_search")) ||
	    (disallowed &&
	     sdk_tools_has(sdk_tools_list(gb, disallowed), "web_search")) ||
	    ((!mode || strcmp(mode, "bypassPermissions")) &&
	     (!rules ||
	      !sdk_tools_has(sdk_tools_list(gb, rules), "web_search"))))
		rc = rc ? rc : set(data, "web_search", "false", false);
	allowed = sdk_tools_allowed(gb, tools, disallowed, fy_seq_empty);
	if (fy_is_sequence(allowed) && fy_empty(allowed))
		rc = rc ? rc : set(data, "tools", "false", false);
	return rc;
}

static int sdk_queue_setting(void *data, const char *key, const char *value,
			     bool quoted)
{
	struct fyai_cfg *cfg = data;
	int rc;

	rc = fyai_config_queue_set(cfg, key, value, quoted);
	if (rc)
		fyai_cfg_error(cfg, "claude-sdk: cannot set %s", key);
	return rc;
}

static int sdk_session_setting(void *data, const char *key, const char *value,
			       bool quoted)
{
	struct fyai_ctx *ctx = data;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic v;
	int rc;

	v = quoted ? fy_value(gb, value) : parse_json_string(gb, value);
	rc = fy_is_valid(v) ? fyai_config_invocation_set(ctx, key, v) : -1;
	if (rc)
		fyai_error(ctx, "claude-sdk: cannot set %s for the session",
			   key);
	return rc;
}

int fyai_cmd_claude_sdk_early(struct fyai_cfg *cfg, fy_generic args)
{
	fy_generic v, stored;
	const char *resume, *sid, *mode;
	const char *prompt, *path, *encoded, *tool, *config, *why, *text;
	char cwd[PATH_MAX], from[sizeof(SDK_BRANCH_PREFIX) + SDK_ID_MAX + 1];
	char *last;
	bool fork, named;
	char id[SDK_ID_MAX + 1], name[sizeof(SDK_BRANCH_PREFIX) + SDK_ID_MAX];
	char *file = NULL;
	char opt[64];
	bool valid;
	size_t i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(sdk_refused); i++) {
		v = fy_get(args, sdk_refused[i], fy_invalid);
		if (fy_is_invalid(v) || fy_equal(v, false))
			continue;
		fyai_cmd_long_name(sdk_refused[i], opt, sizeof(opt));
		fyai_cfg_error(cfg, "claude-sdk: --%s is not supported", opt);
		return -1;
	}
	mode = fy_get(args, "permission_mode", (const char *)NULL);
	fyai_cfg_error_check(cfg, !mode || sdk_mode_valid(mode), err,
			     "claude-sdk: permission mode '%s' is not supported",
			     mode);
	tool = fy_get(args, "permission_prompt_tool", (const char *)NULL);
	fyai_cfg_error_check(cfg, !tool || !strcmp(tool, "stdio"), err,
			     "claude-sdk: the permission prompt tool '%s' is not "
			     "supported; only stdio is", tool);
	config = fy_get(args, "mcp_config", (const char *)NULL);
	if (config) {
		v = sdk_mcp_servers(cfg->gb, config, &why);
		fyai_cfg_error_check(cfg, fy_is_valid(v), err,
				     "claude-sdk: --mcp-config: %s", why);
	}

	text = fy_get(args, "json_schema", (const char *)NULL);
	if (text) {
		v = parse_json_string(cfg->gb, text);
		fyai_cfg_error_check(cfg, fy_is_mapping(v), err,
				     "claude-sdk: --json-schema is not a JSON "
				     "object");
	}
	fyai_cfg_error_check(cfg, fy_get(args, "max_budget_usd", 1.0) > 0, err,
			     "claude-sdk: --max-budget-usd must be positive");

	resume = fy_get(args, "resume", (const char *)NULL);
	sid = fy_get(args, "session_id", (const char *)NULL);
	fork = fy_get(args, "fork_session", false);
	/* --continue takes the last session of this directory, if there is one. */
	if (!resume && fy_get(args, "continue", false) &&
	    getcwd(cwd, sizeof(cwd))) {
		last = fyai_peek_branch_pick(cfg->arena_dir, cfg->root_spec, cwd,
					     false, SDK_BRANCH_PREFIX);
		if (last) {
			snprintf(from, sizeof(from), "%s", last);
			free(last);
			resume = from + strlen(SDK_BRANCH_PREFIX);
		}
	}
	fyai_cfg_error_check(cfg, !(resume && sid) || fork, err,
			     "claude-sdk: --resume and --session-id exclude "
			     "each other");
	fyai_cfg_error_check(cfg, !fork || resume, err,
			     "claude-sdk: --fork-session needs a session to "
			     "fork");
	if (fork) {
		/* The fork is a new session that starts from the resumed one. */
		valid = sdk_id_valid(resume);
		fyai_cfg_error_check(cfg, valid, err,
				     "claude-sdk: invalid session ID '%s'", resume);
		cfg->fresh_session_from = strdup(fy_sprintfa("%s%s",
						 SDK_BRANCH_PREFIX, resume));
		fyai_cfg_error_check(cfg, cfg->fresh_session_from, err,
				     "claude-sdk: cannot keep the session name");
		resume = NULL;
	}
	/*
	 * A named branch (-b, $FYAI_BRANCH) is the session, whatever its name:
	 * one that exists continues, one that does not starts at that name.
	 */
	named = !resume && !sid && cfg->branch_explicit && cfg->branch;
	if (named) {
		rc = fyai_peek_arena_config(cfg->arena_dir, cfg->branch,
					    cfg->root_spec, cfg->gb, &stored,
					    NULL, NULL, NULL);
		if (rc || fy_is_invalid(stored)) {
			/* A new session starts from the configuration of HEAD. */
			cfg->branch_explicit = false;
			cfg->fresh_session = true;
			cfg->fresh_session_name = strdup(cfg->branch);
			fyai_cfg_error_check(cfg, cfg->fresh_session_name, err,
					     "claude-sdk: cannot keep the "
					     "session name");
		}
		goto system_prompt;
	}
	if (resume || sid) {
		valid = sdk_id_valid(resume ? resume : sid);
		fyai_cfg_error_check(cfg, valid, err,
				     "claude-sdk: invalid session ID '%s'",
				     resume ? resume : sid);
		snprintf(id, sizeof(id), "%s", resume ? resume : sid);
	} else {
		valid = sdk_uuid(id, sizeof(id));
		fyai_cfg_error_check(cfg, valid, err,
				     "claude-sdk: cannot make a session ID");
	}
	snprintf(name, sizeof(name), "%s%s", SDK_BRANCH_PREFIX, id);
	if (resume) {
		/* A stored session has a configuration, as each branch has. */
		rc = fyai_peek_arena_config(cfg->arena_dir, name, cfg->root_spec,
					    cfg->gb, &stored, NULL, NULL, NULL);
		fyai_cfg_error_check(cfg, !rc && fy_is_valid(stored), err,
				     "claude-sdk: no session with ID '%s'", id);
		rc = fyai_cfg_set_branch(cfg, name);
		if (rc)		/* fyai_cfg_set_branch() says why */
			goto err;
	} else {
		cfg->fresh_session = true;
		cfg->fresh_session_name = strdup(name);
		fyai_cfg_error_check(cfg, cfg->fresh_session_name, err,
				     "claude-sdk: cannot keep the session name");
	}

system_prompt:
	prompt = fy_get(args, "system_prompt", (const char *)NULL);
	path = fy_get(args, "system_prompt_file", (const char *)NULL);
	if (path) {
		file = read_text_file(path);
		fyai_cfg_error_check(cfg, file, err,
				     "claude-sdk: cannot read the system prompt "
				     "file '%s'", path);
		prompt = file;
	}
	if (prompt && *prompt) {
		/* A JSON string keeps the line breaks that YAML would fold. */
		encoded = emit_json_string(cfg->gb, fy_value(cfg->gb, prompt));
		rc = encoded ? fyai_config_queue_set(cfg, "system_prompt",
						     encoded, false) : -1;
		fyai_cfg_error_check(cfg, !rc, err,
				     "claude-sdk: cannot set the system prompt");
	}
	free(file);
	file = NULL;

	rc = sdk_settings(cfg->gb, args, sdk_queue_setting, cfg);
	if (rc)		/* the setter says why */
		goto err;
	return 0;
err:
	free(file);
	return -1;
}

int fyai_cmd_claude_sdk_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	(void)args;
	cfg->interactive = false;
	cfg->protocol_stdout = true;
	return 0;
}

static void sdk_send(struct sdk_server *srv, fy_generic frame)
{
	if (srv->failed)
		return;
	if (fy_is_invalid(frame) || jsonrpc_conn_send_frame(srv->conn, frame)) {
		fyai_error(srv->ctx, "claude-sdk: cannot send a frame");
		srv->failed = true;
	}
}

static void sdk_respond(struct sdk_server *srv, fy_generic request_id,
			fy_generic response, const char *error)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);

	if (error)
		sdk_send(srv, fy_mapping(gb,
			"type", "control_response",
			"response", fy_mapping(gb,
				"subtype", "error",
				"request_id", request_id,
				"error", error)));
	else
		sdk_send(srv, fy_mapping(gb,
			"type", "control_response",
			"response", fy_mapping(gb,
				"subtype", "success",
				"request_id", request_id,
				"response", fy_is_valid(response) ?
					response : fy_map_empty)));
}


/* ---- the names of tools that the SDK sees ------------------------------- */

/*
 * The Claude Code name and input of a fyai call, for a call that has an
 * equivalent, else the call itself. The input is in @gb.
 */
static const char *sdk_tool_view(struct fy_generic_builder *gb,
				 const char *name, fy_generic args,
				 fy_generic *inputp)
{
	fy_generic v;

	if (!fy_is_mapping(args))
		args = fy_map_empty;
	*inputp = args;
	/* The shell tool is exec_command on the wire and shell in fyai. */
	if (!strcmp(name, "exec_command") || !strcmp(name, "shell")) {
		v = fy_mapping(gb, "command", fy_get(args, "command", ""));
		if (fy_is_valid(fy_get(args, "timeout", fy_invalid)))
			v = fy_assoc(gb, v, "timeout",
				     fy_get(args, "timeout", fy_invalid));
		*inputp = v;
		return "Bash";
	}
	if (!strcmp(name, "read_file")) {
		v = fy_mapping(gb, "file_path", fy_get(args, "path", ""));
		if (fy_is_valid(fy_get(args, "offset", fy_invalid)))
			v = fy_assoc(gb, v, "offset",
				     fy_get(args, "offset", fy_invalid));
		if (fy_is_valid(fy_get(args, "limit", fy_invalid)))
			v = fy_assoc(gb, v, "limit",
				     fy_get(args, "limit", fy_invalid));
		*inputp = v;
		return "Read";
	}
	if (!strcmp(name, "write_file")) {
		*inputp = fy_mapping(gb,
			"file_path", fy_get(args, "path", ""),
			"content", fy_get(args, "content", ""));
		return "Write";
	}
	if (!strcmp(name, "agent")) {
		v = fy_mapping(gb,
			"description", fy_get(args, "description", ""),
			"prompt", fy_get(args, "task", ""));
		if (fy_is_string(fy_get(args, "persona", fy_invalid)))
			v = fy_assoc(gb, v, "subagent_type",
				     fy_get(args, "persona", fy_invalid));
		*inputp = v;
		return "Task";
	}
	if (!strcmp(name, "structured_output"))
		return "StructuredOutput";
	return name;
}

/*
 * The fyai arguments of @input, an input of the SDK for the call of @name
 * whose arguments were @args. A field that the SDK does not see stays.
 */
static fy_generic sdk_tool_unview(struct fy_generic_builder *gb,
				  const char *name, fy_generic args,
				  fy_generic input)
{
	static const struct {
		const char *tool, *from, *to;
	} fields[] = {
		{ "exec_command",	"command",	"command" },
		{ "exec_command",	"timeout",	"timeout" },
		{ "read_file",		"file_path",	"path" },
		{ "read_file",		"offset",	"offset" },
		{ "read_file",		"limit",	"limit" },
		{ "write_file",		"file_path",	"path" },
		{ "write_file",		"content",	"content" },
		{ "agent",		"description",	"description" },
		{ "agent",		"prompt",	"task" },
		{ "agent",		"subagent_type", "persona" },
	};
	fy_generic v;
	bool mapped = false;
	size_t i;

	if (!fy_is_mapping(args))
		args = fy_map_empty;
	if (!strcmp(name, "shell"))
		name = "exec_command";
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		if (strcmp(fields[i].tool, name))
			continue;
		mapped = true;
		v = fy_get(input, fields[i].from, fy_invalid);
		if (fy_is_valid(v))
			args = fy_assoc(gb, args, fields[i].to, v);
	}
	return mapped ? args : input;
}

/* Map the tool_use blocks of @content to the names that the SDK sees. */
static fy_generic sdk_blocks_view(struct fy_generic_builder *gb,
				  fy_generic content)
{
	fy_generic out, block, input;
	const char *name, *view;

	out = fy_seq_empty;
	fy_foreach(block, content) {
		if (fy_equal(fy_get(block, "type", fy_invalid), "tool_use")) {
			name = fy_get(block, "name", "");
			view = sdk_tool_view(gb, name,
					     fy_get(block, "input", fy_invalid),
					     &input);
			if (view != name)
				block = fy_assoc(gb, block, "name", view,
						 "input", input);
		}
		out = fy_append(gb, out, block);
	}
	return out;
}

/* ---- permission rules ------------------------------------------------------ */

static void sdk_rules_free(struct sdk_rules *rules)
{
	size_t i;

	for (i = 0; i < rules->n; i++) {
		free(rules->v[i].tool);
		free(rules->v[i].spec);
	}
	free(rules->v);
	rules->v = NULL;
	rules->n = 0;
}

/* Parse rules such as `Read`, `Bash(git log:*)` and `mcp__s__t`. */
static int sdk_rules_parse(const char *list, struct sdk_rules *rules)
{
	struct sdk_rule *v, *r;
	const char *p = list, *item, *open;
	size_t len;

	while ((item = sdk_list_next(&p, &len))) {
		v = realloc(rules->v, (rules->n + 1) * sizeof(*v));
		if (!v)
			return -1;
		rules->v = v;
		r = &v[rules->n];
		open = memchr(item, '(', len);
		if (open && item[len - 1] == ')') {
			r->tool = strndup(item, (size_t)(open - item));
			r->spec = strndup(open + 1, len - (size_t)(open - item) - 2);
		} else {
			r->tool = strndup(item, len);
			r->spec = NULL;
		}
		rules->n++;
		if (!r->tool || (open && !r->spec))
			return -1;
	}
	return 0;
}

/* True when the tool of a rule names @claude_name or @fyai_name. */
static bool sdk_rule_names(const char *tool, const char *claude_name,
			   const char *fyai_name)
{
	size_t i, j;

	if (!strcmp(tool, claude_name) || !strcmp(tool, fyai_name))
		return true;
	for (i = 0; i < ARRAY_SIZE(sdk_tool_map); i++) {
		if (strcmp(sdk_tool_map[i].claude, tool))
			continue;
		for (j = 0; j < ARRAY_SIZE(sdk_tool_map[i].fyai) &&
			    sdk_tool_map[i].fyai[j]; j++)
			if (!strcmp(sdk_tool_map[i].fyai[j], fyai_name))
				return true;
	}
	return false;
}

/*
 * True when a rule covers the call. The text of a Bash rule is a command, and
 * `PREFIX:*` covers each command that starts with PREFIX; the text of a rule
 * of a file is a pattern of the path. A rule of another tool with text covers
 * no call: fyai does not know what its text means.
 */
static bool sdk_rule_matches(const struct sdk_rule *r, const char *claude_name,
			     const char *fyai_name, fy_generic input)
{
	const char *subject;
	size_t len;

	if (!sdk_rule_names(r->tool, claude_name, fyai_name))
		return false;
	if (!r->spec)
		return true;
	if (!strcmp(claude_name, "Bash")) {
		subject = fy_get(input, "command", "");
		len = strlen(r->spec);
		if (len >= 2 && !strcmp(r->spec + len - 2, ":*"))
			return !strncmp(subject, r->spec, len - 2);
		return !fnmatch(r->spec, subject, 0);
	}
	if (!strcmp(claude_name, "Read") || !strcmp(claude_name, "Write")) {
		subject = fy_get(input, "file_path", "");
		return !fnmatch(r->spec, subject, 0);
	}
	return false;
}

static const struct sdk_rule *sdk_rules_match(const struct sdk_rules *rules,
					      const char *claude_name,
					      const char *fyai_name,
					      fy_generic input)
{
	size_t i;

	for (i = 0; i < rules->n; i++)
		if (sdk_rule_matches(&rules->v[i], claude_name, fyai_name,
				     input))
			return &rules->v[i];
	return NULL;
}

/* ---- control requests to the SDK ----------------------------------------- */

static void sdk_pending_free(struct sdk_pending *p)
{
	free(p->rpc_id_json);
	free(p);
}

/*
 * Send @request as a control request; @done gets its response. The pending
 * entry is returned to fill in before the response can arrive.
 */
static struct sdk_pending *sdk_request(struct sdk_server *srv,
				       fy_generic request, sdk_done_fn done)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);
	struct sdk_pending *p;

	if (srv->failed)
		return NULL;
	p = calloc(1, sizeof(*p));
	if (!p) {
		fyai_error(srv->ctx, "claude-sdk: cannot keep a request");
		return NULL;
	}
	snprintf(p->id, sizeof(p->id), "fyai_%llu", ++srv->requests);
	p->done = done;
	p->rpc_id = fy_invalid;
	sdk_send(srv, fy_mapping(gb,
		"type", "control_request",
		"request_id", p->id,
		"request", request));
	if (srv->failed) {
		sdk_pending_free(p);
		return NULL;
	}
	p->next = srv->pending;
	srv->pending = p;
	return p;
}

/* Complete the request that a control_response answers. */
static void sdk_response(struct sdk_server *srv, fy_generic frame)
{
	struct sdk_pending **pp, *p;
	fy_generic resp;
	const char *id;

	resp = fy_get(frame, "response", fy_invalid);
	id = fy_get(resp, "request_id", "");
	for (pp = &srv->pending; (p = *pp); pp = &p->next)
		if (!strcmp(p->id, id))
			break;
	if (!p) {
		fyai_debug(srv->ctx, "claude-sdk: a response to '%s' is "
			   "ignored", id);
		return;
	}
	*pp = p->next;
	if (fy_equal(fy_get(resp, "subtype", fy_invalid), "error"))
		p->done(srv, p, fy_invalid, fy_get(resp, "error",
						   "the SDK reported an error"));
	else
		p->done(srv, p, fy_get(resp, "response", fy_map_empty), NULL);
	sdk_pending_free(p);
}

/* ---- hooks ---------------------------------------------------------------- */

static void sdk_hooks_free(struct sdk_server *srv)
{
	size_t i;

	for (i = 0; i < srv->nhooks; i++) {
		if (!srv->hooks[i].every)
			regfree(&srv->hooks[i].re);
		free(srv->hooks[i].event);
		free(srv->hooks[i].callback_id);
	}
	free(srv->hooks);
	srv->hooks = NULL;
	srv->nhooks = 0;
}

/*
 * Keep the hooks of an initialize request: for each event, matchers that
 * name the callbacks of the SDK. A matcher is a regular expression of the
 * tool name; none, "" or "*" matches every tool. Returns an error text.
 */
static const char *sdk_hooks_parse(struct sdk_server *srv, fy_generic hooks)
{
	struct sdk_hook *v, *h;
	fy_generic event, matchers, m, ids, id;
	const char *matcher, *ev;

	fy_foreach_key_value(event, matchers, hooks) {
		ev = fy_castp(&event, "");
		fy_foreach(m, matchers) {
			matcher = fy_get(m, "matcher", (const char *)NULL);
			ids = fy_get(m, "hookCallbackIds", fy_seq_empty);
			fy_foreach(id, ids) {
				v = realloc(srv->hooks, (srv->nhooks + 1) *
					    sizeof(*v));
				if (!v)
					return "cannot keep the hooks";
				srv->hooks = v;
				h = &v[srv->nhooks];
				memset(h, 0, sizeof(*h));
				h->every = !matcher || !*matcher ||
					   !strcmp(matcher, "*");
				if (!h->every &&
				    regcomp(&h->re, fy_sprintfa("^(%s)$", matcher),
					    REG_EXTENDED | REG_NOSUB))
					return fy_gb_intern_string(
						fyai_ctx_transient_gb(srv->ctx),
						fy_sprintfa("the matcher '%s' "
							    "is not a regular "
							    "expression",
							    matcher));
				srv->nhooks++;
				h->event = strdup(ev);
				h->callback_id = strdup(fy_castp(&id, ""));
				if (!h->event || !h->callback_id)
					return "cannot keep the hooks";
			}
		}
	}
	return NULL;
}

static bool sdk_hook_matches(const struct sdk_hook *h, const char *event,
			     const char *tool)
{
	if (strcmp(h->event, event))
		return false;
	return h->every || !tool || !regexec(&h->re, tool, 0, NULL, 0);
}

static bool sdk_has_hooks(const struct sdk_server *srv, const char *event)
{
	size_t i;

	for (i = 0; i < srv->nhooks; i++)
		if (!strcmp(srv->hooks[i].event, event))
			return true;
	return false;
}

/* The fields that each hook input has, with @event. */
static fy_generic sdk_hook_input(struct sdk_server *srv, const char *event)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);
	char cwd[PATH_MAX];

	return fy_mapping(gb,
		"hook_event_name", event,
		"session_id", srv->session_id,
		"transcript_path", "",
		"cwd", getcwd(cwd, sizeof(cwd)) ? cwd : "",
		"permission_mode", srv->permission_mode);
}

/* Send a hook callback for @hook with @input; @done gets the output. */
static struct sdk_pending *sdk_hook_call(struct sdk_server *srv,
					 const struct sdk_hook *hook,
					 fy_generic input,
					 const char *tool_use_id,
					 sdk_done_fn done)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);

	return sdk_request(srv, fy_mapping(gb,
		"subtype", "hook_callback",
		"callback_id", hook->callback_id,
		"input", input,
		"tool_use_id", tool_use_id ? fy_value(gb, tool_use_id) :
			fy_null), done);
}

static void sdk_text_add(char **dst, const char *text)
{
	char *joined;

	if (!text || !*text)
		return;
	if (!*dst) {
		*dst = strdup(text);
		return;
	}
	if (asprintf(&joined, "%s\n%s", *dst, text) < 0)
		return;
	free(*dst);
	*dst = joined;
}

/* ---- decisions on tool calls ---------------------------------------------- */

static void sdk_decision_free(struct sdk_decision *d)
{
	free(d->claude_name);
	free(d->fyai_name);
	free(d->args_json);
	free(d->input_json);
	free(d->tool_use_id);
	free(d->result_json);
	free(d->reason);
	free(d->updated_json);
	free(d->context);
	free(d->stop_reason);
	free(d);
}

/* Release a decision that the gate has and no response waits for. */
static void sdk_decision_release(struct sdk_server *srv,
				 struct sdk_decision *d)
{
	struct sdk_decision **pp;

	if (!d->answered || d->waiting)
		return;
	for (pp = &srv->decisions; *pp; pp = &(*pp)->next)
		if (*pp == d) {
			*pp = d->next;
			break;
		}
	sdk_decision_free(d);
}

static void sdk_allow(struct sdk_server *srv, struct sdk_decision *d)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic args = fy_invalid, input;

	if (d->updated_json) {
		input = parse_json_string(gb, d->updated_json);
		args = sdk_tool_unview(gb, d->fyai_name,
				       parse_json_string(gb, d->args_json),
				       input);
	}
	d->answered = true;
	fyai_tool_gate_allow(ctx, d->gate_id, d->index, args);
}

static void sdk_deny(struct sdk_server *srv, struct sdk_decision *d,
		     const char *message, bool stop)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);

	if (srv->busy)
		srv->denials = fy_append(gb, srv->denials, fy_mapping(gb,
			"tool_name", d->claude_name,
			"tool_use_id", d->tool_use_id,
			"tool_input", parse_json_string(gb, d->input_json)));
	d->answered = true;
	fyai_tool_gate_deny(srv->ctx, d->gate_id, d->index, message, stop);
}

/*
 * What a call of the fyai tool @name can change. A tool with no entry, such
 * as an MCP tool, can change anything.
 */
static int sdk_tool_effect(const char *name)
{
	const struct fyai_tool_def *def = fyai_tool_find(name);

	return def ? def->effect : FYAI_TOOL_EFFECT_PROCESS;
}

static void sdk_permission_done(struct sdk_server *srv, struct sdk_pending *p,
				fy_generic response, const char *error)
{
	struct sdk_decision *d = p->decision;
	fy_generic updated;
	const char *json;

	d->waiting--;
	if (error) {
		sdk_deny(srv, d, fy_sprintfa("the permission request failed: %s",
					     error), false);
	} else if (fy_equal(fy_get(response, "behavior", fy_invalid),
			    "allow")) {
		updated = fy_get(response, "updatedInput", fy_invalid);
		if (fy_is_mapping(updated)) {
			json = emit_json_string(fyai_ctx_transient_gb(srv->ctx),
						updated);
			free(d->updated_json);
			d->updated_json = json ? strdup(json) : NULL;
		}
		sdk_allow(srv, d);
	} else {
		sdk_deny(srv, d, fy_get(response, "message",
					"permission was denied"),
			 fy_get(response, "interrupt", false));
	}
	sdk_decision_release(srv, d);
}

/*
 * Decide by the permission mode, the rules and the SDK, after the hooks
 * have not decided. @ask says that a hook asked for a question.
 */
static void sdk_permission(struct sdk_server *srv, struct sdk_decision *d,
			   bool ask)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);
	struct sdk_pending *p;
	fy_generic input;
	const char *mode = srv->permission_mode;

	input = parse_json_string(gb, d->updated_json ? d->updated_json :
				  d->input_json);
	if (!ask) {
		if (!strcmp(mode, "bypassPermissions") ||
		    sdk_rules_match(&srv->allow, d->claude_name, d->fyai_name,
				    input) ||
		    sdk_tool_effect(d->fyai_name) == FYAI_TOOL_EFFECT_NONE ||
		    (!strcmp(mode, "acceptEdits") &&
		     sdk_tool_effect(d->fyai_name) ==
				FYAI_TOOL_EFFECT_FILES)) {
			sdk_allow(srv, d);
			return;
		}
	}
	/* With no one to ask, a call that needs permission does not run. */
	if (!strcmp(mode, "dontAsk") || !srv->can_use_tool) {
		sdk_deny(srv, d, fy_sprintfa("permission to use %s was not "
					     "granted", d->claude_name), false);
		return;
	}
	p = sdk_request(srv, fy_mapping(gb,
		"subtype", "can_use_tool",
		"tool_name", d->claude_name,
		"input", input,
		"permission_suggestions", fy_seq_empty,
		"blocked_path", fy_null,
		"tool_use_id", d->tool_use_id,
		"decision_reason", d->reason ? fy_value(gb, d->reason) : fy_null),
		sdk_permission_done);
	if (!p) {
		sdk_deny(srv, d, "the permission request could not be sent",
			 true);
		return;
	}
	p->decision = d;
	d->waiting++;
}

/* Fold the output of a PreToolUse hook into the decision. */
static void sdk_pre_hook_done(struct sdk_server *srv, struct sdk_pending *p,
			      fy_generic out, const char *error)
{
	struct sdk_decision *d = p->decision;
	fy_generic specific, updated;
	const char *decision, *reason, *json;

	d->waiting--;
	if (error) {
		d->verdict = SDK_DENY;
		sdk_text_add(&d->reason, fy_sprintfa("a hook failed: %s",
						     error));
	} else {
		specific = fy_get(out, "hookSpecificOutput", fy_invalid);
		decision = fy_get(specific, "permissionDecision", "");
		reason = fy_get(specific, "permissionDecisionReason",
				fy_get(out, "reason", ""));
		/* The old form: a decision of the output. */
		if (!*decision && fy_equal(fy_get(out, "decision", fy_invalid),
					   "block"))
			decision = "deny";
		if (!strcmp(decision, "deny")) {
			d->verdict = SDK_DENY;
			sdk_text_add(&d->reason, reason);
		} else if (!strcmp(decision, "ask") && d->verdict != SDK_DENY) {
			d->verdict = SDK_ASK;
			sdk_text_add(&d->reason, reason);
		} else if (!strcmp(decision, "allow") &&
			   d->verdict == SDK_NONE) {
			d->verdict = SDK_ALLOW;
		}
		updated = fy_get(specific, "updatedInput", fy_invalid);
		if (fy_is_mapping(updated)) {
			json = emit_json_string(fyai_ctx_transient_gb(srv->ctx),
						updated);
			free(d->updated_json);
			d->updated_json = json ? strdup(json) : NULL;
		}
		if (fy_equal(fy_get(out, "continue", fy_invalid), false)) {
			d->stop = true;
			sdk_text_add(&d->stop_reason,
				     fy_get(out, "stopReason", ""));
		}
	}
	if (d->waiting)
		return;
	if (d->stop)
		sdk_deny(srv, d, d->stop_reason ? d->stop_reason :
			 "a hook stopped the turn", true);
	else if (d->verdict == SDK_DENY)
		sdk_deny(srv, d, d->reason ? d->reason :
			 "a hook denied the tool call", false);
	else if (d->verdict == SDK_ALLOW)
		sdk_allow(srv, d);
	else
		sdk_permission(srv, d, d->verdict == SDK_ASK);
	sdk_decision_release(srv, d);
}

static struct sdk_decision *sdk_decision_new(struct sdk_server *srv,
					     uint64_t id, size_t index,
					     fy_generic call, bool after)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct sdk_decision *d;
	const char *name, *view, *args_json, *input_json;
	fy_generic args, input;

	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->gate_id = id;
	d->index = index;
	d->after = after;
	name = fyai_tool_call_name(ctx, call);
	args = fyai_tool_call_args(ctx, call);
	if (!fy_is_mapping(args))
		args = fy_map_empty;
	view = sdk_tool_view(gb, name, args, &input);
	args_json = emit_json_string(gb, args);
	input_json = emit_json_string(gb, input);
	d->claude_name = strdup(view);
	d->fyai_name = strdup(name);
	d->args_json = strdup(args_json ? args_json : "{}");
	d->input_json = strdup(input_json ? input_json : "{}");
	d->tool_use_id = strdup(fyai_tool_call_id(ctx, call));
	if (!d->claude_name || !d->fyai_name || !d->args_json ||
	    !d->input_json || !d->tool_use_id) {
		sdk_decision_free(d);
		return NULL;
	}
	d->next = srv->decisions;
	srv->decisions = d;
	return d;
}

static void sdk_gate_before(struct fyai_ctx *ctx, uint64_t id, size_t index,
			    fy_generic call, void *data)
{
	struct sdk_server *srv = data;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const struct sdk_rule *rule;
	struct sdk_decision *d;
	struct sdk_pending *p;
	fy_generic input, hook_input;
	size_t i;

	d = sdk_decision_new(srv, id, index, call, false);
	if (!d) {
		fyai_tool_gate_deny(ctx, id, index,
				    "the tool call could not be decided", true);
		return;
	}
	input = parse_json_string(gb, d->input_json);
	rule = sdk_rules_match(&srv->deny, d->claude_name, d->fyai_name, input);
	if (rule) {
		sdk_deny(srv, d, fy_sprintfa("permission to use %s is denied",
					     d->claude_name), false);
		sdk_decision_release(srv, d);
		return;
	}
	hook_input = fy_assoc(gb, sdk_hook_input(srv, "PreToolUse"),
			      "tool_name", d->claude_name,
			      "tool_input", input,
			      "tool_use_id", d->tool_use_id);
	d->waiting++;		/* the loop below holds the decision */
	for (i = 0; i < srv->nhooks; i++) {
		if (!sdk_hook_matches(&srv->hooks[i], "PreToolUse",
				      d->claude_name))
			continue;
		p = sdk_hook_call(srv, &srv->hooks[i], hook_input,
				  d->tool_use_id, sdk_pre_hook_done);
		if (!p)
			continue;
		p->decision = d;
		d->waiting++;
	}
	if (--d->waiting)
		return;
	sdk_permission(srv, d, false);
	sdk_decision_release(srv, d);
}

static void sdk_post_answer(struct sdk_server *srv, struct sdk_decision *d)
{
	d->answered = true;
	fyai_tool_gate_done(srv->ctx, d->gate_id, d->index,
			    d->stop ? (d->stop_reason ? d->stop_reason :
				       "a hook stopped the turn") : d->context,
			    d->stop);
}

static void sdk_post_hook_done(struct sdk_server *srv, struct sdk_pending *p,
			       fy_generic out, const char *error)
{
	struct sdk_decision *d = p->decision;
	fy_generic specific;

	d->waiting--;
	if (!error) {
		specific = fy_get(out, "hookSpecificOutput", fy_invalid);
		sdk_text_add(&d->context, fy_get(specific, "additionalContext",
						 ""));
		if (fy_equal(fy_get(out, "decision", fy_invalid), "block"))
			sdk_text_add(&d->context, fy_get(out, "reason", ""));
		if (fy_equal(fy_get(out, "continue", fy_invalid), false)) {
			d->stop = true;
			sdk_text_add(&d->stop_reason,
				     fy_get(out, "stopReason", ""));
		}
	} else {
		fyai_warning(srv->ctx, "claude-sdk: a PostToolUse hook failed: "
			     "%s", error);
	}
	if (d->waiting)
		return;
	sdk_post_answer(srv, d);
	sdk_decision_release(srv, d);
}

static void sdk_gate_after(struct fyai_ctx *ctx, uint64_t id, size_t index,
			   fy_generic call, fy_generic result, bool ok,
			   void *data)
{
	struct sdk_server *srv = data;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct sdk_decision *d;
	struct sdk_pending *p;
	fy_generic input;
	const char *event;
	char *cause;
	size_t i;

	event = ok ? "PostToolUse" : "PostToolUseFailure";
	if (!sdk_has_hooks(srv, event)) {
		fyai_tool_gate_done(ctx, id, index, NULL, false);
		return;
	}
	d = sdk_decision_new(srv, id, index, call, true);
	if (!d) {
		fyai_tool_gate_done(ctx, id, index, NULL, false);
		return;
	}
	input = fy_assoc(gb, sdk_hook_input(srv, event),
			 "tool_name", d->claude_name,
			 "tool_input", parse_json_string(gb, d->input_json),
			 "tool_use_id", d->tool_use_id);
	if (ok) {
		input = fy_assoc(gb, input, "tool_response", result);
	} else {
		cause = fyai_tool_error_cause(result);
		input = fy_assoc(gb, input, "error",
				 cause ? cause : "the tool call failed");
		free(cause);
	}
	d->waiting++;
	for (i = 0; i < srv->nhooks; i++) {
		if (!sdk_hook_matches(&srv->hooks[i], event, d->claude_name))
			continue;
		p = sdk_hook_call(srv, &srv->hooks[i], input, d->tool_use_id,
				  sdk_post_hook_done);
		if (!p)
			continue;
		p->decision = d;
		d->waiting++;
	}
	if (--d->waiting)
		return;
	sdk_post_answer(srv, d);
	sdk_decision_release(srv, d);
}

static const struct fyai_tool_gate_ops sdk_gate_ops = {
	.before = sdk_gate_before,
	.after = sdk_gate_after,
};

/* True when the session decides on tool calls: else they run as they come. */
static bool sdk_gate_needed(const struct sdk_server *srv)
{
	if (strcmp(srv->permission_mode, "bypassPermissions") || srv->deny.n)
		return true;
	return sdk_has_hooks(srv, "PreToolUse") ||
	       sdk_has_hooks(srv, "PostToolUse") ||
	       sdk_has_hooks(srv, "PostToolUseFailure");
}

/* ---- hooks that the front end waits for ----------------------------------- */

static void sdk_wait_free(struct sdk_wait *w)
{
	free(w->reason);
	free(w->context);
	free(w->stop_reason);
	memset(w, 0, sizeof(*w));
}

static void sdk_wait_done(struct sdk_server *srv, struct sdk_pending *p,
			  fy_generic out, const char *error)
{
	struct sdk_wait *w = p->wait;
	fy_generic specific;

	w->waiting--;
	if (error) {
		fyai_warning(srv->ctx, "claude-sdk: a hook failed: %s", error);
		return;
	}
	specific = fy_get(out, "hookSpecificOutput", fy_invalid);
	sdk_text_add(&w->context, fy_get(specific, "additionalContext", ""));
	if (fy_equal(fy_get(out, "decision", fy_invalid), "block")) {
		w->block = true;
		sdk_text_add(&w->reason, fy_get(out, "reason", ""));
	}
	if (fy_equal(fy_get(out, "continue", fy_invalid), false)) {
		w->stop = true;
		sdk_text_add(&w->stop_reason, fy_get(out, "stopReason", ""));
	}
}

/*
 * Call the hooks of @event with @input and wait for their outputs. This
 * runs the event loop: call it only from the loop of the command, between
 * turns.
 */
static int sdk_hooks_wait(struct sdk_server *srv, const char *event,
			  fy_generic input, struct sdk_wait *w)
{
	struct fyai_event_loop *el = fyai_ctx_loop(srv->ctx);
	struct sdk_pending *p;
	size_t i;
	int rc;

	memset(w, 0, sizeof(*w));
	for (i = 0; i < srv->nhooks; i++) {
		if (!sdk_hook_matches(&srv->hooks[i], event, NULL))
			continue;
		p = sdk_hook_call(srv, &srv->hooks[i], input, NULL,
				  sdk_wait_done);
		if (!p)
			continue;
		p->wait = w;
		w->waiting++;
	}
	while (w->waiting && !srv->failed && !jsonrpc_conn_closed(srv->conn)) {
		rc = fyai_event_loop_step(el, -1);
		fyai_error_check(srv->ctx, rc >= 0, err,
				 "claude-sdk: the event loop stopped");
	}
	return w->waiting ? -1 : 0;
err:
	return -1;
}

/* ---- MCP servers of the SDK ----------------------------------------------- */

static void sdk_mcp_done(struct sdk_server *srv, struct sdk_pending *p,
			 fy_generic response, const char *error)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);
	fy_generic reply, id;

	if (!p->tunnel->conn)
		return;
	reply = fy_get(response, "mcp_response", fy_invalid);
	if (error && p->rpc_id_json) {
		/* The request of the server still needs its answer. */
		id = parse_json_string(gb, p->rpc_id_json);
		reply = fy_mapping(gb, "jsonrpc", "2.0", "id", id,
			"error", fy_mapping(gb, "code", -32603LL,
					    "message", error));
	}
	if (fy_is_mapping(reply))
		jsonrpc_conn_tunnel_receive(p->tunnel->conn, reply);
}

/* Carry a frame of fyai to an MCP server of the SDK. */
static int sdk_mcp_send(void *userdata, fy_generic frame)
{
	struct sdk_tunnel *t = userdata;
	struct sdk_server *srv = t->srv;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);
	struct sdk_pending *p;
	fy_generic id;
	const char *json;

	p = sdk_request(srv, fy_mapping(gb,
		"subtype", "mcp_message",
		"server_name", t->name,
		"message", frame), sdk_mcp_done);
	if (!p)
		return -1;
	p->tunnel = t;
	id = fy_get(frame, "id", fy_invalid);
	if (fy_is_valid(id) && fy_is_valid(fy_get(frame, "method", fy_invalid))) {
		json = emit_json_string(gb, id);
		p->rpc_id_json = json ? strdup(json) : NULL;
	}
	return 0;
}

static void sdk_respond_init(struct sdk_server *srv, fy_generic id);

/* The servers have settled: answer the initialize that waits for them. */
static void sdk_mcp_started(struct sdk_server *srv)
{
	srv->mcp_starting = false;
	if (!srv->init_id)
		return;
	sdk_respond_init(srv, fy_value(fyai_ctx_transient_gb(srv->ctx),
				       srv->init_id));
	free(srv->init_id);
	srv->init_id = NULL;
}

/* Start the servers of --mcp-config and wait until each has settled. */
static int sdk_mcp_start(struct sdk_server *srv, const char *config)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	struct sdk_tunnel *t;
	fy_generic servers, name, server, fconf;
	const char *nm, *type, *why;
	int rc;

	srv->mcp_starting = true;
	servers = sdk_mcp_servers(gb, config, &why);
	fyai_error_check(ctx, fy_is_valid(servers), err,
			 "claude-sdk: --mcp-config: %s", why);
	fy_foreach_key_value(name, server, servers) {
		nm = fy_castp(&name, "");
		type = fy_get(server, "type", "stdio");
		if (!strcmp(type, "sdk")) {
			t = calloc(1, sizeof(*t));
			fyai_error_check(ctx, t, err,
					 "claude-sdk: cannot keep MCP server "
					 "'%s'", nm);
			t->srv = srv;
			t->name = strdup(nm);
			t->next = srv->tunnels;
			srv->tunnels = t;
			fyai_error_check(ctx, t->name, err,
					 "claude-sdk: cannot keep MCP server "
					 "'%s'", nm);
			rc = fyai_mcp_add_tunnel(ctx, nm, sdk_mcp_send, t,
						 &t->conn);
		} else if (!strcmp(type, "http")) {
			fconf = fy_mapping(gb,
				"transport", "streamable-http",
				"endpoint", fy_get(server, "url", ""));
			rc = fyai_mcp_add(ctx, nm, fconf);
		} else {
			fconf = fy_mapping(gb,
				"command", fy_get(server, "command", ""),
				"args", fy_get(server, "args", fy_seq_empty));
			if (fy_is_mapping(fy_get(server, "env", fy_invalid)))
				fconf = fy_assoc(gb, fconf, "env",
						 fy_get(server, "env", fy_invalid));
			rc = fyai_mcp_add(ctx, nm, fconf);
		}
		if (rc)		/* the MCP client says why */
			goto err;
	}
	while (!fyai_mcp_settled(ctx) && !srv->failed) {
		rc = fyai_event_loop_step(el, -1);
		fyai_error_check(ctx, rc >= 0, err,
				 "claude-sdk: the event loop stopped");
	}
	fyai_mcp_publish_tools(ctx);
	rc = fyai_request_state_apply(ctx);
	fyai_error_check(ctx, !rc, err,
			 "claude-sdk: cannot apply the MCP tools");
	sdk_mcp_started(srv);
	return 0;
err:
	sdk_mcp_started(srv);
	return -1;
}

/* The MCP servers as the SDK reports them, in @gb. */
static fy_generic sdk_mcp_status(struct sdk_server *srv,
				 struct fy_generic_builder *gb)
{
	fy_generic rows, row, out;
	const char *state;

	out = fy_seq_empty;
	rows = fyai_mcp_status_data(srv->ctx, gb);
	fy_foreach(row, rows) {
		state = fy_get(row, "state", "");
		out = fy_append(gb, out, fy_mapping(gb,
			"name", fy_get(row, "server", ""),
			"status", !strcmp(state, "ready") ? "connected" :
				!strcmp(state, "connecting") ? "pending" :
				"failed"));
	}
	return out;
}

/* ---- the model of the session --------------------------------------------- */

/* Store @model for the session, or the default model for NULL. */
static const char *sdk_set_model(struct sdk_server *srv, const char *model)
{
	struct fyai_ctx *ctx = srv->ctx;
	fy_generic saved;
	bool own;
	int rc;

	if (srv->busy)
		return "a turn is running";
	/* No model is the model that the session started with. */
	if (!model)
		model = srv->model;
	if (!model)
		return "the session has no model to return to";
	/*
	 * The request is in the transient builder: use it, and release only a
	 * builder that this change made.
	 */
	own = !ctx->transient_gb;
	rc = fyai_setup_transient_builder(ctx);
	if (rc)
		return "cannot prepare the change";
	/* As in Claude Code, the model changes for this process alone. */
	saved = ctx->cfg->config_session;
	rc = fyai_config_invocation_set(ctx, "model",
					fy_value(ctx->transient_gb, model));
	if (!rc)
		rc = fyai_auth_resolve(ctx);
	if (!rc)
		rc = fyai_request_state_apply(ctx);
	if (rc) {
		/* A model that cannot be used leaves the session as it was. */
		ctx->cfg->config_session = saved;
		if (fyai_config_rederive(ctx) || fyai_auth_resolve(ctx) ||
		    fyai_request_state_apply(ctx))
			fyai_error(ctx, "claude-sdk: cannot restore the model");
	}
	if (own)
		fyai_cleanup_transient_builder(ctx);
	else
		ctx->transient_autorelease = true;
	if (!rc)
		return NULL;
	return fy_gb_intern_string(ctx->cfg->gb,
			fy_sprintfa("the model '%s' cannot be used", model));
}

/*
 * Make the agents of the SDK personas of the session: the description, the
 * prompt, the tools and the model of each. Returns an error text.
 */
static const char *sdk_agents(struct sdk_server *srv, fy_generic agents)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic name, agent, persona, tools, tool, allow;
	const char *nm, *model, *word;
	int rc;

	fy_foreach_key_value(name, agent, agents) {
		nm = fy_castp(&name, "");
		persona = fy_mapping(gb,
			"description", fy_get(agent, "description", ""),
			"system_prompt", fy_get(agent, "prompt", ""));
		/* The tools of the agent, by the names of fyai. */
		tools = fy_get(agent, "tools", fy_invalid);
		if (fy_is_sequence(tools)) {
			allow = fy_seq_empty;
			fy_foreach(tool, tools) {
				word = fy_castp(&tool, "");
				allow = sdk_tools_add(gb, allow, word,
						      strlen(word));
			}
			persona = fy_assoc(gb, persona, "tools", allow);
		} else if (fy_is_valid(tools) && !fy_is_null(tools)) {
			return fy_gb_intern_string(gb,
				fy_sprintfa("agent '%s' gives tools that are "
					    "not a list", nm));
		}
		/* A model alias of Claude Code names no model of fyai. */
		model = fy_get(agent, "model", "");
		if (*model && strcmp(model, "inherit") && strcmp(model, "sonnet") &&
		    strcmp(model, "opus") && strcmp(model, "haiku"))
			persona = fy_assoc(gb, persona, "model", model);
		rc = fyai_config_invocation_set(ctx,
			fy_sprintfa("agent/personas/%s", nm), persona);
		if (rc)
			return fy_gb_intern_string(gb,
				fy_sprintfa("agent '%s' is not a valid "
					    "persona", nm));
		srv->agents = fy_append(ctx->cfg->gb, srv->agents,
					fy_value(ctx->cfg->gb, nm));
	}
	/* The agent tool lists the personas of the request. */
	rc = fyai_request_state_apply(ctx);
	return rc ? "the agents cannot be offered" : NULL;
}

/* The context window as the SDK reports it. */
static fy_generic sdk_context_usage(struct sdk_server *srv,
				    struct fy_generic_builder *gb)
{
	struct fyai_ctx *ctx = srv->ctx;
	long long used, window;

	used = ctx->last_call_input + ctx->last_call_output;
	window = fyai_context_window(ctx);
	return fy_mapping(gb,
		"categories", fy_sequence(gb, fy_mapping(gb,
			"name", "Messages", "tokens", used,
			"color", "#808080")),
		"totalTokens", used,
		"maxTokens", window,
		"rawMaxTokens", window,
		"percentage", window > 0 ? 100.0 * (double)used /
				(double)window : 0.0,
		"model", ctx->cfg->model ? ctx->cfg->model : "",
		"isAutoCompactEnabled", false,
		"memoryFiles", fy_seq_empty,
		"mcpTools", fy_seq_empty,
		"agents", fy_seq_empty);
}

static void sdk_respond_init(struct sdk_server *srv, fy_generic id)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);

	sdk_respond(srv, id, fy_mapping(gb,
		"commands", fy_seq_empty,
		"output_style", "default",
		"available_output_styles", fy_sequence(gb, "default"),
		"models", fy_seq_empty), NULL);
}

static void sdk_control(struct sdk_server *srv, fy_generic frame)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);
	fy_generic id, req, model;
	const char *subtype, *mode, *why;

	id = fy_get(frame, "request_id", fy_invalid);
	req = fy_get(frame, "request", fy_invalid);
	subtype = fy_get(req, "subtype", "");
	if (!fy_is_string(id)) {
		fyai_warning(srv->ctx, "claude-sdk: a control request has no ID");
		return;
	}
	if (!strcmp(subtype, "initialize")) {
		if (srv->initialized) {
			sdk_respond(srv, id, fy_invalid, "already initialized");
			return;
		}
		if (fy_is_mapping(fy_get(req, "hooks", fy_invalid))) {
			why = sdk_hooks_parse(srv, fy_get(req, "hooks",
							  fy_invalid));
			if (why) {
				sdk_hooks_free(srv);
				sdk_respond(srv, id, fy_invalid, why);
				return;
			}
		}
		if (fy_is_mapping(fy_get(req, "agents", fy_invalid))) {
			why = sdk_agents(srv, fy_get(req, "agents", fy_invalid));
			if (why) {
				sdk_respond(srv, id, fy_invalid, why);
				return;
			}
		}
		srv->forward_text = fy_get(req, "forwardSubagentText", false);
		srv->initialized = true;
		/* As Claude Code, answer once the MCP servers have connected. */
		if (srv->mcp_starting) {
			srv->init_id = strdup(fy_castp(&id, ""));
			if (!srv->init_id)
				sdk_respond(srv, id, fy_invalid,
					    "cannot keep the request");
			return;
		}
		sdk_respond_init(srv, id);
		return;
	}
	if (!strcmp(subtype, "interrupt")) {
		if (srv->busy)
			srv->ctx->interrupt_pending = true;
		sdk_respond(srv, id, fy_invalid, NULL);
		return;
	}
	if (!strcmp(subtype, "set_permission_mode")) {
		mode = fy_get(req, "mode", "");
		if (!sdk_mode_valid(mode)) {
			sdk_respond(srv, id, fy_invalid,
				    fy_sprintfa("fyai does not support "
						"permission mode '%s'", mode));
			return;
		}
		srv->permission_mode = fy_gb_intern_string(srv->ctx->cfg->gb,
							   mode);
		sdk_respond(srv, id, fy_invalid, NULL);
		return;
	}
	if (!strcmp(subtype, "set_model")) {
		model = fy_get(req, "model", fy_invalid);
		why = sdk_set_model(srv, fy_is_string(model) ?
				    fy_castp(&model, "") : NULL);
		sdk_respond(srv, id, fy_invalid, why);
		return;
	}
	if (!strcmp(subtype, "get_context_usage")) {
		sdk_respond(srv, id, sdk_context_usage(srv, gb), NULL);
		return;
	}
	if (!strcmp(subtype, "mcp_status")) {
		sdk_respond(srv, id, fy_mapping(gb,
			"mcpServers", sdk_mcp_status(srv, gb)), NULL);
		return;
	}
	sdk_respond(srv, id, fy_invalid,
		    fy_sprintfa("fyai does not support the control request '%s'",
				subtype));
}

static void sdk_images_free(struct sdk_image *images)
{
	struct sdk_image *img;

	while ((img = images)) {
		images = img->next;
		free(img->media_type);
		free(img->name);
		free(img->data);
		free(img);
	}
}

/*
 * Keep the image blocks of the content of a user message, as the Claude
 * Agent SDK sends them: {type: image, source: {type: base64, media_type,
 * data}}. A block of another source is reported and left out. Return 0, or -1
 * after a report.
 */
static int sdk_user_images(struct sdk_server *srv, fy_generic content,
			   struct sdk_image **headp)
{
	struct sdk_image **tail = headp, *img;
	fy_generic block, source;
	const char *type, *data;

	if (!fy_is_sequence(content))
		return 0;
	fy_foreach(block, content) {
		if (!fy_equal(fy_get(block, "type", fy_invalid), "image"))
			continue;
		source = fy_get(block, "source", fy_invalid);
		type = fy_get(source, "type", "");
		data = fy_get(source, "data", (const char *)NULL);
		if (strcmp(type, "base64") || !data) {
			fyai_warning(srv->ctx, "claude-sdk: an image block with "
				     "the source '%s' is not supported; it is "
				     "left out", *type ? type : "?");
			continue;
		}
		img = calloc(1, sizeof(*img));
		fyai_error_check(srv->ctx, img, err,
				 "claude-sdk: cannot keep an image");
		img->data = fyai_base64_decode(data, &img->len);
		img->media_type = strdup(fy_get(source, "media_type", ""));
		img->name = strdup(fy_get(block, "name", "image"));
		fyai_error_check(srv->ctx, img->data && img->media_type &&
				 img->name, err_free,
				 "claude-sdk: an image block holds no valid "
				 "base64 data");
		*tail = img;
		tail = &img->next;
	}
	return 0;
err_free:
	sdk_images_free(img);
err:
	return -1;
}

/* The text of the content of a user message, or NULL. Owned by the caller. */
static char *sdk_user_text(struct sdk_server *srv, fy_generic content)
{
	struct response_buffer out = { 0 };
	fy_generic block, type;
	const char *s;

	if (fy_is_string(content))
		return strdup(fy_castp(&content, ""));
	if (!fy_is_sequence(content))
		return NULL;
	fy_foreach(block, content) {
		type = fy_get(block, "type", fy_invalid);
		if (fy_equal(type, "image"))	/* see sdk_user_images() */
			continue;
		if (!fy_equal(type, "text")) {
			fyai_warning(srv->ctx,
				     "claude-sdk: a '%s' block of a user message "
				     "is not supported; it is left out",
				     fy_str(type) ? fy_str(type) : "?");
			continue;
		}
		s = fy_get(block, "text", "");
		if ((out.len && response_buffer_append_data(&out, "\n", 1)) ||
		    response_buffer_append_data(&out, s, strlen(s))) {
			free(out.data);
			return NULL;
		}
	}
	return out.data ? out.data : strdup("");
}

/*
 * The regular file that the mention @PATH at @start names, owned by the
 * caller, or NULL. A mention can end with punctuation of the sentence.
 */
static char *sdk_mention_path(const char *start, size_t len)
{
	const char *home = getenv("HOME");
	struct stat st;
	char *path;

	for (; len; len--) {
		if (len > 1 && start[0] == '~' && start[1] == '/' && home)
			path = strdup(fy_sprintfa("%s%.*s", home,
						  (int)len - 1, start + 1));
		else
			path = strndup(start, len);
		if (!path)
			return NULL;
		if (!stat(path, &st) && S_ISREG(st.st_mode))
			return path;
		free(path);
		if (!strchr(".,;:!?)'\"", start[len - 1]))
			break;
	}
	return NULL;
}

/*
 * As Claude Code, add the content of each file that the prompt names as
 * @PATH, as a call of Read and its result. The result is in the transient
 * builder, or @prompt when it names no file.
 */
static const char *sdk_mentions(struct sdk_server *srv, const char *prompt)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const struct fyai_tool_def *read = fyai_tool_find("read_file");
	struct response_buffer out = { 0 };
	const char *p, *json, *head, *res = prompt;
	char *path, *content;
	size_t len;
	bool ok;
	int rc;

	if (!read || !read->run_text)
		return prompt;
	rc = response_buffer_append_data(&out, prompt, strlen(prompt));
	for (p = prompt; !rc && (p = strchr(p, '@')); p++) {
		if (p != prompt && !strchr(" \t\r\n", p[-1]))
			continue;
		len = strcspn(p + 1, " \t\r\n");
		path = len ? sdk_mention_path(p + 1, len) : NULL;
		if (!path)
			continue;
		content = read->run_text(ctx, fy_mapping(gb, "path", path), &ok);
		json = emit_json_string(gb, fy_mapping(gb, "file_path", path));
		head = json ? fy_gb_intern_string(gb, fy_sprintfa(
			"\n\nCalled the Read tool with the following input: "
			"%s\n\nResult of calling the Read tool:\n", json)) :
			NULL;
		if (content && ok && head)
			rc = response_buffer_append_data(&out, head,
							 strlen(head)) ||
			     response_buffer_append_data(&out, content,
							 strlen(content));
		else
			fyai_warning(ctx, "claude-sdk: cannot read %s, which "
				     "the prompt names", path);
		free(content);
		free(path);
	}
	if (rc)
		fyai_error(ctx, "claude-sdk: cannot add the files that the "
			   "prompt names");
	else if (out.len != strlen(prompt))
		res = fy_gb_intern_string(gb, out.data);
	free(out.data);
	return res ? res : prompt;
}

static void sdk_user(struct sdk_server *srv, fy_generic frame)
{
	struct sdk_input *in;
	struct sdk_image *images = NULL;
	fy_generic origin, content;
	const char *json;
	char *text;

	content = fy_get_at_path(frame, "message", "content");
	text = sdk_user_text(srv, content);
	if (sdk_user_images(srv, content, &images)) {
		sdk_images_free(images);
		free(text);
		return;
	}
	if ((!text || !*text) && !images) {
		fyai_warning(srv->ctx, "claude-sdk: a user message has no text");
		free(text);
		return;
	}
	in = calloc(1, sizeof(*in));
	if (!in) {
		fyai_error(srv->ctx, "claude-sdk: cannot queue a user message");
		sdk_images_free(images);
		free(text);
		srv->failed = true;
		return;
	}
	in->text = text ? text : strdup("");
	in->images = images;
	if (!in->text) {
		fyai_error(srv->ctx, "claude-sdk: cannot queue a user message");
		sdk_images_free(images);
		free(in);
		free(text);
		srv->failed = true;
		return;
	}
	in->composed = fy_get(frame, "client_composed", false);
	origin = fy_get(frame, "origin", fy_invalid);
	if (fy_is_mapping(origin)) {
		json = emit_json_string(fyai_ctx_transient_gb(srv->ctx), origin);
		in->origin_json = json ? strdup(json) : NULL;
	}
	*srv->tail = in;
	srv->tail = &in->next;
}

static void sdk_frame(struct jsonrpc_conn *conn, fy_generic frame, void *data)
{
	struct sdk_server *srv = data;
	fy_generic type;

	(void)conn;
	type = fy_get(frame, "type", fy_invalid);
	if (fy_equal(type, "control_request"))
		sdk_control(srv, frame);
	else if (fy_equal(type, "user"))
		sdk_user(srv, frame);
	else if (fy_equal(type, "control_response"))
		sdk_response(srv, frame);
	else if (!fy_equal(type, "control_cancel_request"))
		fyai_debug(srv->ctx, "claude-sdk: a '%s' frame is ignored",
			   fy_str(type) ? fy_str(type) : "?");
}

/* A new UUID in the transient builder: a frame takes it at once. */
static const char *sdk_new_uuid(struct sdk_server *srv)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);
	const char *s;
	char id[40];

	if (!gb || !sdk_uuid(id, sizeof(id)))
		return "";
	s = fy_gb_intern_string(gb, id);
	return s ? s : "";
}

/* Usage as an Anthropic usage object, from fyai usage counts. */
static fy_generic sdk_usage(struct fy_generic_builder *gb, long long input,
			    long long output, long long cached,
			    long long cache_write)
{
	return fy_mapping(gb,
		"input_tokens", input,
		"output_tokens", output,
		"cache_read_input_tokens", cached,
		"cache_creation_input_tokens", cache_write);
}

/* Keep the text blocks of an answer as the text of the result. */
static void sdk_keep_text(struct sdk_server *srv, fy_generic content)
{
	struct response_buffer out = { 0 };
	fy_generic block;
	const char *s;

	fy_foreach(block, content) {
		if (!fy_equal(fy_get(block, "type", fy_invalid), "text"))
			continue;
		s = fy_get(block, "text", "");
		if (response_buffer_append_data(&out, s, strlen(s))) {
			free(out.data);
			return;
		}
	}
	if (!out.data)
		return;
	free(srv->result_text);
	srv->result_text = out.data;
}

static void sdk_stream_event(struct sdk_server *srv, fy_generic event)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);

	sdk_send(srv, fy_mapping(gb,
		"type", "stream_event",
		"uuid", sdk_new_uuid(srv),
		"session_id", srv->session_id,
		"event", event,
		"parent_tool_use_id", fy_null));
}

/*
 * Stream a chunk of answer text as the events of an Anthropic message: the
 * first chunk of a response opens the message and its text block.
 */
static void sdk_text(struct fyai_ctx *ctx, const char *text, size_t len,
		     void *data)
{
	struct sdk_server *srv = data;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	char msgid[48];

	if (!srv->stream_open) {
		snprintf(msgid, sizeof(msgid), "msg_%s", sdk_new_uuid(srv));
		sdk_stream_event(srv, fy_mapping(gb,
			"type", "message_start",
			"message", fy_mapping(gb,
				"id", msgid,
				"type", "message",
				"role", "assistant",
				"model", ctx->cfg->model ? ctx->cfg->model : "",
				"content", fy_seq_empty,
				"stop_reason", fy_null,
				"stop_sequence", fy_null,
				"usage", sdk_usage(gb, 0, 0, 0, 0))));
		sdk_stream_event(srv, fy_mapping(gb,
			"type", "content_block_start",
			"index", 0LL,
			"content_block", fy_mapping(gb, "type", "text",
						    "text", "")));
		srv->stream_open = true;
	}
	sdk_stream_event(srv, fy_mapping(gb,
		"type", "content_block_delta",
		"index", 0LL,
		"delta", fy_mapping(gb, "type", "text_delta",
				    "text", fy_sprintfa("%.*s", (int)len, text))));
}

/* Close the streamed message of a response that has ended. */
static void sdk_stream_close(struct sdk_server *srv, bool tools,
			     long long output)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(srv->ctx);

	if (!srv->stream_open)
		return;
	srv->stream_open = false;
	sdk_stream_event(srv, fy_mapping(gb,
		"type", "content_block_stop", "index", 0LL));
	sdk_stream_event(srv, fy_mapping(gb,
		"type", "message_delta",
		"delta", fy_mapping(gb,
			"stop_reason", tools ? "tool_use" : "end_turn",
			"stop_sequence", fy_null),
		"usage", fy_mapping(gb, "output_tokens", output)));
	sdk_stream_event(srv, fy_mapping(gb, "type", "message_stop"));
}

static bool sdk_has_tool_use(fy_generic content)
{
	fy_generic block;

	fy_foreach(block, content)
		if (fy_equal(fy_get(block, "type", fy_invalid), "tool_use"))
			return true;
	return false;
}

/* Only the tool_use and tool_result blocks of @content. */
static fy_generic sdk_blocks_tools(struct fy_generic_builder *gb,
				   fy_generic content)
{
	fy_generic out, block;

	out = fy_seq_empty;
	fy_foreach(block, content)
		if (fy_any_equal(fy_get(block, "type", fy_invalid),
				 "tool_use", "tool_result"))
			out = fy_append(gb, out, block);
	return out;
}

/*
 * Send the messages of one step: of the turn, or, with @parent, of the
 * sub-agent that the tool use @parent started. A sub-agent shows its text
 * only when the SDK asked for it.
 */
static void sdk_send_step(struct sdk_server *srv, fy_generic messages,
			  fy_generic usage, const char *model,
			  const char *parent)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic blocks, m, content, parent_id;
	char msgid[48];
	const char *role;

	blocks = fyai_messages_blocks(ctx, messages);
	if (fy_is_invalid(blocks)) {
		fyai_error(ctx, "claude-sdk: cannot convert a step of the turn");
		if (!parent)
			srv->failed = true;
		return;
	}
	parent_id = parent ? fy_value(gb, parent) : fy_null;
	fy_foreach(m, blocks) {
		role = fy_get(m, "role", "");
		content = sdk_blocks_view(gb, fy_get(m, "content", fy_seq_empty));
		if (parent && !srv->forward_text) {
			content = sdk_blocks_tools(gb, content);
			if (fy_empty(content))
				continue;
		}
		if (!strcmp(role, "assistant")) {
			if (!parent) {
				srv->num_turns++;
				sdk_stream_close(srv, sdk_has_tool_use(content),
						 fy_get(usage, "output", 0LL));
				sdk_keep_text(srv, content);
			}
			snprintf(msgid, sizeof(msgid), "msg_%s",
				 sdk_new_uuid(srv));
			sdk_send(srv, fy_mapping(gb,
				"type", "assistant",
				"message", fy_mapping(gb,
					"id", msgid,
					"type", "message",
					"role", "assistant",
					"model", model,
					"content", content,
					"stop_reason", fy_null,
					"stop_sequence", fy_null,
					"usage", sdk_usage(gb,
						fy_get(usage, "input", 0LL),
						fy_get(usage, "output", 0LL),
						fy_get(usage, "cached", 0LL),
						fy_get(usage, "cache_write", 0LL))),
				"parent_tool_use_id", parent_id,
				"session_id", srv->session_id,
				"uuid", sdk_new_uuid(srv)));
		} else {
			sdk_send(srv, fy_mapping(gb,
				"type", "user",
				"message", fy_mapping(gb,
					"role", "user",
					"content", content),
				"parent_tool_use_id", parent_id,
				"session_id", srv->session_id,
				"uuid", sdk_new_uuid(srv)));
		}
	}
}

static void sdk_observe(struct fyai_ctx *ctx, fy_generic turn, void *data)
{
	sdk_send_step(data, fy_get(turn, "messages", fy_seq_empty),
		      fy_get(fyai_turn_meta(turn), "usage", fy_invalid),
		      ctx->cfg->model ? ctx->cfg->model : "", NULL);
}

/* A step of a sub-agent belongs to the tool use that started it. */
static void sdk_agent_observe(struct fyai_ctx *ctx, fy_generic call,
			      fy_generic step, void *data)
{
	fy_generic model;
	const char *id;

	id = fyai_tool_call_id(ctx, call);
	if (!id || !*id)
		return;
	model = fy_get(step, "model", fy_invalid);
	sdk_send_step(data, fy_get(step, "messages", fy_seq_empty),
		      fy_get(step, "usage", fy_invalid), fy_castp(&model, ""),
		      id);
}

static void sdk_send_init(struct sdk_server *srv)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic tools, tool, names, input;
	const char *name;
	char cwd[PATH_MAX];

	names = fy_seq_empty;
	tools = ctx->cfg->enable_tools ? make_tools_filtered(ctx) :
		fy_seq_empty;
	fy_foreach(tool, tools) {
		name = fy_get(fy_get(tool, "function", fy_invalid), "name", "");
		names = fy_append(gb, names,
				  sdk_tool_view(gb, name, fy_invalid, &input));
	}
	fy_foreach(tool, fyai_mcp_tools(ctx))
		names = fy_append(gb, names,
			fy_get(fy_get(tool, "function", fy_invalid), "name", ""));
	sdk_send(srv, fy_mapping(gb,
		"type", "system",
		"subtype", "init",
		"cwd", getcwd(cwd, sizeof(cwd)) ? cwd : "",
		"session_id", srv->session_id,
		"tools", names,
		"mcp_servers", sdk_mcp_status(srv, gb),
		"model", ctx->cfg->model ? ctx->cfg->model : "",
		"permissionMode", srv->permission_mode,
		"slash_commands", fy_seq_empty,
		"apiKeySource", "none",
		"claude_code_version", FYAI_CLAUDE_SDK_VERSION,
		"output_style", "default",
		"agents", srv->agents,
		"skills", fy_seq_empty,
		"plugins", fy_seq_empty,
		"uuid", sdk_new_uuid(srv)));
}

static long long sdk_elapsed_ms(const struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)(now.tv_sec - start->tv_sec) * 1000 +
	       (now.tv_nsec - start->tv_nsec) / 1000000;
}

/* Add a user message of @text to the conversation and store it. */
static int sdk_add_user(struct sdk_server *srv, const char *text)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic user, turn;
	char *shown;
	int rc;

	/*
	 * The images that the user message carries are pending. They go with
	 * this message and no other; a message that the program writes carries
	 * none.
	 */
	shown = fyai_media_pending_display(ctx, text);
	user = fyai_media_pending_any(ctx) ?
		fyai_make_user_message(ctx, text) :
		fyai_make_instruction_message(ctx, text);
	turn = fyai_turn_append(ctx, ctx->last_message, fy_sequence(gb, user));
	turn = fyai_output_record(ctx, turn, FYAI_OUTPUT_USER,
				  shown ? shown : text);
	free(shown);
	fyai_error_check(ctx, fy_is_valid(turn), err,
			 "claude-sdk: cannot add the user message");
	ctx->last_message = fy_gb_internalize(ctx->gb, turn);
	fyai_error_check(ctx, fy_is_valid(ctx->last_message), err,
			 "claude-sdk: cannot keep the user message");
	rc = fyai_publish_state(ctx);
	fyai_error_check(ctx, !rc, err,
			 "claude-sdk: cannot store the user message");
	return 0;
err:
	return -1;
}

/*
 * Run the model on the conversation and store what it did. Returns the cause
 * of a turn that stopped, owned by the caller, or NULL.
 */
static char *sdk_run(struct sdk_server *srv, bool *okp)
{
	struct fyai_ctx *ctx = srv->ctx;
	fy_generic turn, diag, value;
	char *cause = NULL;
	int rc;

	*okp = false;
	ctx->turn_stop = FYAI_TURN_STOP_NONE;
	ctx->tool_gate = sdk_gate_needed(srv) ? &sdk_gate_ops : NULL;
	ctx->tool_gate_data = srv;
	srv->busy = true;
	turn = fyai_run_turn(ctx, ctx->last_message);
	srv->busy = false;
	ctx->tool_gate = NULL;
	ctx->tool_gate_data = NULL;
	/* A response that streamed and failed still ends its message. */
	sdk_stream_close(srv, false, 0);
	diag = fy_generic_get_diag(turn);
	if (fy_is_valid(diag) && !fy_is_null(diag)) {
		cause = strdup(fy_castp(&diag, ""));
		fyai_error(ctx, "%s", cause ? cause : "the turn stopped");
		value = fy_generic_indirect_get_value(turn);
	} else {
		value = turn;
	}
	if (fy_is_valid(value)) {
		ctx->last_message = value;
		rc = fyai_publish_state(ctx);
		fyai_error_check(ctx, !rc, out,
				 "claude-sdk: cannot store the turn");
	}
	*okp = fy_is_valid(value) && !cause;
out:
	return cause;
}

/* A model that answers in prose is reminded of the structured output. */
#define SDK_OUTPUT_RETRIES	3

/* Run one turn for the user message @in and send its result. */
static void sdk_turn(struct sdk_server *srv, const struct sdk_input *in)
{
	const char *text = in->text;
	const struct sdk_image *img;
	struct fyai_ctx *ctx = srv->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	struct fy_generic_builder *gb;
	struct timespec start;
	struct sdk_wait w;
	fy_generic input, frame;
	long long in0, out0, cached0, write0, ms;
	const char *prompt, *subtype = NULL, *reason;
	double cost0;
	char *cause = NULL;
	bool ok, again, api_error;
	long status;
	int rc, retries;

	clock_gettime(CLOCK_MONOTONIC, &start);
	in0 = ctx->usage_input;
	out0 = ctx->usage_output;
	cached0 = ctx->usage_cached;
	write0 = ctx->usage_cache_write;
	cost0 = ctx->usage_cost;
	srv->num_turns = 0;
	free(srv->result_text);
	srv->result_text = NULL;
	srv->denials = fy_seq_empty;
	ctx->structured_output = fy_invalid;
	memset(&w, 0, sizeof(w));

	ok = false;
	rc = fyai_setup_transient_builder(ctx);
	fyai_error_check(ctx, !rc, done, "claude-sdk: cannot prepare the turn");
	gb = fyai_ctx_transient_gb(ctx);
	sdk_send_init(srv);

	prompt = text;
	if (sdk_has_hooks(srv, "UserPromptSubmit")) {
		input = fy_assoc(gb, sdk_hook_input(srv, "UserPromptSubmit"),
				 "prompt", text);
		rc = sdk_hooks_wait(srv, "UserPromptSubmit", input, &w);
		fyai_error_check(ctx, !rc, done,
				 "claude-sdk: the hooks of the prompt did not "
				 "answer");
		if (w.block || w.stop) {
			cause = strdup(fy_sprintfa("a hook blocked the prompt%s%s",
				w.reason || w.stop_reason ? ": " : "",
				w.reason ? w.reason :
				w.stop_reason ? w.stop_reason : ""));
			goto done;
		}
		if (w.context)
			prompt = fy_sprintfa("%s\n\n%s", text, w.context);
		sdk_wait_free(&w);
	}
	if (!in->composed)
		prompt = sdk_mentions(srv, prompt);
	/* The attachments of the message wait for sdk_add_user(). */
	fyai_media_pending_clear(ctx);
	for (img = in->images; img; img = img->next) {
		rc = fyai_media_attach_bytes(ctx, img->data, img->len,
					     *img->media_type ?
						img->media_type : NULL,
					     img->name);
		if (rc) {	/* the attach says why */
			fyai_media_pending_clear(ctx);
			cause = strdup("an image of the message cannot be "
				       "attached");
			goto done;
		}
	}
	rc = sdk_add_user(srv, prompt);
	if (rc)		/* sdk_add_user() says why */
		goto done;

	for (again = false, retries = 0; ; ) {
		cause = sdk_run(srv, &ok);
		if (!ok)
			break;
		if (fy_is_mapping(ctx->output_schema) &&
		    !fy_is_valid(ctx->structured_output) &&
		    retries < SDK_OUTPUT_RETRIES) {
			retries++;
			rc = sdk_add_user(srv, "Give the answer now: call the "
					  "structured_output tool with the "
					  "answer as its arguments.");
			if (rc) {	/* sdk_add_user() says why */
				ok = false;
				break;
			}
			continue;
		}
		if (!sdk_has_hooks(srv, "Stop"))
			break;
		input = fy_assoc(gb, sdk_hook_input(srv, "Stop"),
				 "stop_hook_active", (_Bool)again,
				 "last_assistant_message",
				 srv->result_text ? srv->result_text : "");
		rc = sdk_hooks_wait(srv, "Stop", input, &w);
		/* A Stop hook that blocks gives the model a reason to go on. */
		if (rc || w.stop || !w.block) {
			sdk_wait_free(&w);
			break;
		}
		rc = sdk_add_user(srv, w.reason ? w.reason : "Continue.");
		sdk_wait_free(&w);
		if (rc) {	/* sdk_add_user() says why */
			ok = false;
			break;
		}
		again = true;
	}

done:
	sdk_wait_free(&w);
	/* A turn that asked for structured output ends with it. */
	if (ok && fy_is_mapping(ctx->output_schema) &&
	    !fy_is_valid(ctx->structured_output)) {
		ok = false;
		cause = strdup("the model gave no structured output");
		subtype = "error_max_structured_output_retries";
	}
	/*
	 * A failed provider request ends a loop that itself completed: the
	 * result has subtype success, the reason in its text, and no errors.
	 */
	api_error = !ok && !subtype &&
		    ctx->turn_stop == FYAI_TURN_STOP_REQUEST;
	if (api_error) {
		free(cause);
		cause = fyai_diag_string(&cfg->diag);
		subtype = "success";
	}
	if (!ok && !cause)
		cause = fyai_diag_string(&cfg->diag);
	if (!ok && !subtype)
		subtype = ctx->turn_stop == FYAI_TURN_STOP_LIMIT ?
				"error_max_turns" :
			  ctx->turn_stop == FYAI_TURN_STOP_COST ?
				"error_max_budget_usd" :
				"error_during_execution";
	reason = ok ? "completed" : api_error ? "api_error" :
		 ctx->turn_stop == FYAI_TURN_STOP_LIMIT ? "max_turns" : NULL;
	ms = sdk_elapsed_ms(&start);
	gb = fyai_ctx_transient_gb(ctx);
	frame = fy_mapping(gb,
		"type", "result",
		"subtype", ok ? "success" : subtype,
		"is_error", (_Bool)!ok,
		"duration_ms", ms,
		"duration_api_ms", ms,
		"num_turns", srv->num_turns,
		"result", ok ? (srv->result_text ? srv->result_text : "") :
			(cause && *cause ? cause : "the turn did not complete"),
		"stop_reason", ok ? fy_value(gb, "end_turn") : fy_null,
		"session_id", srv->session_id,
		"total_cost_usd", ctx->usage_cost - cost0,
		"usage", sdk_usage(gb,
			ctx->usage_input - in0, ctx->usage_output - out0,
			ctx->usage_cached - cached0,
			ctx->usage_cache_write - write0),
		"permission_denials", fy_is_valid(srv->denials) ?
			srv->denials : fy_seq_empty,
		"uuid", sdk_new_uuid(srv));
	if (reason)
		frame = fy_assoc(gb, frame, "terminal_reason", reason);
	status = fyai_xfer_last_status(ctx);
	if (api_error && status >= 400)
		frame = fy_assoc(gb, frame, "api_error_status", status);
	if (api_error)
		frame = fy_assoc(gb, frame, "errors", fy_seq_empty);
	else if (!ok)
		frame = fy_assoc(gb, frame, "errors", fy_sequence(gb,
			cause && *cause ? cause : "the turn did not complete"));
	if (ok && fy_is_valid(ctx->structured_output))
		frame = fy_assoc(gb, frame, "structured_output",
				 ctx->structured_output);
	/* The origin of the prompt marks the result that answers it. */
	if (in->origin_json)
		frame = fy_assoc(gb, frame, "origin",
				 parse_json_string(gb, in->origin_json));
	sdk_send(srv, frame);
	srv->last_error = !ok;
	free(cause);
	fyai_diag_drain(&cfg->diag);
	fyai_cleanup_transient_builder(ctx);
}

/*
 * Start the conversation of a session that has no exchange with the system
 * prompt of the configuration and the text of --append-system-prompt.
 */
static int sdk_system_seed(struct sdk_server *srv)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const char *text = ctx->cfg->system_prompt;
	fy_generic turn;

	if (srv->append && *srv->append)
		text = fy_sprintfa("%s\n\n%s", text, srv->append);
	turn = fyai_turn_append(ctx, fy_invalid,
		fy_sequence(gb, fyai_make_system_message(ctx, text)));
	turn = fyai_output_record(ctx, turn, FYAI_OUTPUT_SYSTEM, text);
	ctx->last_message = fy_gb_internalize(ctx->gb, turn);
	fyai_error_check(ctx, fy_is_valid(ctx->last_message), err,
			 "claude-sdk: cannot keep the system prompt");
	return 0;
err:
	return -1;
}

/*
 * Append --append-system-prompt to the system prompt of a session that has
 * no exchange. The setup made that prompt, from the configuration, before
 * this runs.
 */
static int sdk_system_append(struct sdk_server *srv)
{
	struct fyai_ctx *ctx = srv->ctx;
	int rc;

	if (!srv->append || !*srv->append)
		return 0;
	if (!ctx->session_unstored) {
		fyai_warning(ctx, "claude-sdk: a resumed session keeps its "
			     "system prompt");
		return 0;
	}
	rc = fyai_setup_transient_builder(ctx);
	fyai_error_check(ctx, !rc, err,
			 "claude-sdk: cannot prepare the system prompt");
	rc = sdk_system_seed(srv);
	fyai_cleanup_transient_builder(ctx);
	return rc;
err:
	return -1;
}

/*
 * /clear starts a new session, as in Claude Code: the conversation_reset
 * frame names the session that ends, and the result the one that starts.
 */
static void sdk_clear(struct sdk_server *srv)
{
	struct fyai_ctx *ctx = srv->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const char *name;
	char *old, *sid;
	int rc;

	sid = strdup(sdk_new_uuid(srv));
	fyai_error_check(ctx, sid && *sid, err,
			 "claude-sdk: cannot name a new session");
	name = fy_sprintfa("%s%s", SDK_BRANCH_PREFIX, sid);
	rc = fyai_ctx_set_branch(ctx, name);
	fyai_error_check(ctx, !rc, err,
			 "claude-sdk: cannot select the new session");
	/* The configuration goes with it; the conversation does not. */
	free(ctx->session_unstored);
	ctx->session_unstored = strdup(name);
	fyai_error_check(ctx, ctx->session_unstored, err,
			 "claude-sdk: cannot keep the new session name");
	ctx->branch_desc = fy_invalid;
	ctx->branch_agent = fy_invalid;
	ctx->branch_prev = fy_invalid;
	old = srv->session_id;
	srv->session_id = sid;
	sid = NULL;
	rc = sdk_system_seed(srv);
	sdk_send(srv, fy_mapping(gb,
		"type", "conversation_reset",
		"new_conversation_id", srv->session_id,
		"uuid", sdk_new_uuid(srv),
		"session_id", old));
	free(old);
	sdk_send(srv, fy_mapping(gb,
		"type", "result",
		"subtype", rc ? "error_during_execution" : "success",
		"is_error", (_Bool)!!rc,
		"duration_ms", 0LL,
		"duration_api_ms", 0LL,
		"num_turns", 0LL,
		"result", "",
		"stop_reason", fy_null,
		"session_id", srv->session_id,
		"total_cost_usd", 0.0,
		"usage", sdk_usage(gb, 0, 0, 0, 0),
		"permission_denials", fy_seq_empty,
		"uuid", sdk_new_uuid(srv)));
	srv->last_error = !!rc;
	fyai_diag_drain(&ctx->cfg->diag);
	fyai_cleanup_transient_builder(ctx);
	return;
err:
	free(sid);
	srv->failed = true;
	fyai_cleanup_transient_builder(ctx);
}

/* True for a prompt that is the command /clear. */
static bool sdk_is_clear(const char *text)
{
	while (*text == ' ' || *text == '\t' || *text == '\n')
		text++;
	if (strncmp(text, "/clear", 6))
		return false;
	for (text += 6; *text == ' ' || *text == '\t' || *text == '\n'; )
		text++;
	return !*text;
}

int fyai_cmd_claude_sdk(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	struct sdk_server srv;
	struct sdk_input *in;
	struct sdk_pending *p;
	struct sdk_decision *d;
	struct sdk_tunnel *t;
	const char *branch, *mode, *text;
	fy_generic allowed, names, tool;
	double budget;
	bool found;
	int rc = -1;

	(void)result;
	memset(&srv, 0, sizeof(srv));
	srv.ctx = ctx;
	srv.tail = &srv.head;
	srv.denials = fy_seq_empty;
	srv.agents = fy_seq_empty;
	fyai_error_check(ctx, el, out, "claude-sdk: no event loop");

	branch = fyai_ctx_branch(ctx);
	/* The ID of an sdk/ branch is its name below the prefix. */
	found = !strncmp(branch, SDK_BRANCH_PREFIX, strlen(SDK_BRANCH_PREFIX));
	srv.session_id = strdup(found ? branch + strlen(SDK_BRANCH_PREFIX) :
				branch);
	fyai_error_check(ctx, srv.session_id, out,
			 "claude-sdk: cannot keep the session ID");
	mode = fyai_cmd_arg_str(call, "permission_mode");
	srv.permission_mode = mode ? mode : "default";
	/* A change of the configuration during the session keeps them. */
	rc = fyai_setup_transient_builder(ctx);
	if (!rc)
		rc = sdk_settings(ctx->transient_gb, call->args,
				  sdk_session_setting, ctx);
	fyai_cleanup_transient_builder(ctx);
	if (rc)		/* the setter says why */
		goto out;
	srv.model = fy_get(ctx->cfg->config_doc, "model", (const char *)NULL);
	if (srv.model)
		srv.model = fy_gb_intern_string(ctx->cfg->gb, srv.model);
	srv.can_use_tool = fyai_cmd_arg_str(call, "permission_prompt_tool") !=
			   NULL;
	text = fyai_cmd_arg_str(call, "allowed_tools");
	rc = text ? sdk_rules_parse(text, &srv.allow) : 0;
	fyai_error_check(ctx, !rc, out, "claude-sdk: cannot keep the rules");
	text = fyai_cmd_arg_str(call, "disallowed_tools");
	rc = text ? sdk_rules_parse(text, &srv.deny) : 0;
	fyai_error_check(ctx, !rc, out, "claude-sdk: cannot keep the rules");

	srv.append = fyai_cmd_arg_str(call, "append_system_prompt");
	rc = sdk_system_append(&srv);
	if (rc)		/* sdk_system_append() says why */
		goto out;

	rc = -1;
	srv.conn = jsonrpc_conn_stdio(ctx, STDOUT_FILENO, STDIN_FILENO, 0,
				      "claude-sdk", NULL);
	fyai_error_check(ctx, srv.conn, out,
			 "claude-sdk: cannot open standard input and output");
	rc = jsonrpc_conn_frames(srv.conn, sdk_frame, &srv);
	fyai_error_check(ctx, !rc, out,
			 "claude-sdk: cannot read standard input");
	ctx->turn_observe = sdk_observe;
	ctx->turn_observe_data = &srv;
	ctx->agent_observe = sdk_agent_observe;
	ctx->agent_observe_data = &srv;

	/* The servers of the SDK answer through the connection, which reads. */
	text = fyai_cmd_arg_str(call, "mcp_config");
	if (text) {
		rc = fyai_setup_transient_builder(ctx);
		fyai_error_check(ctx, !rc, out,
				 "claude-sdk: cannot prepare the MCP servers");
		rc = sdk_mcp_start(&srv, text);
		fyai_cleanup_transient_builder(ctx);
		if (rc)		/* sdk_mcp_start() says why */
			goto out;
	}
	names = fy_seq_empty;
	fy_foreach(tool, fyai_mcp_tools(ctx))
		names = fy_append(ctx->cfg->gb, names,
			fy_get(fy_get(tool, "function", fy_invalid), "name", ""));
	allowed = sdk_tools_allowed(ctx->cfg->gb,
				    fyai_cmd_arg_str(call, "tools"),
				    fyai_cmd_arg_str(call, "disallowed_tools"),
				    names);
	if (fy_is_sequence(allowed) && !fy_empty(allowed)) {
		/* The tools of a request were made at setup: make them again. */
		ctx->tools_allow = fy_gb_internalize(ctx->cfg->gb, allowed);
		rc = fyai_setup_transient_builder(ctx);
		if (!rc) {
			ctx->tools_spec = fy_invalid;
			rc = fyai_request_state_apply(ctx);
		}
		fyai_cleanup_transient_builder(ctx);
		fyai_error_check(ctx, !rc, out,
				 "claude-sdk: cannot limit the tools");
	}

	/* The structured_output tool takes the schema of the answer. */
	text = fyai_cmd_arg_str(call, "json_schema");
	if (text) {
		ctx->output_schema = parse_json_string(ctx->cfg->gb, text);
		rc = fyai_setup_transient_builder(ctx);
		if (!rc) {
			ctx->tools_spec = fy_invalid;
			rc = fyai_request_state_apply(ctx);
		}
		fyai_cleanup_transient_builder(ctx);
		fyai_error_check(ctx, !rc && fy_is_mapping(ctx->output_schema),
				 out, "claude-sdk: cannot ask for the structured "
				 "output");
	}
	/* The budget is of the whole invocation, as of a query of the SDK. */
	budget = fy_get(call->args, "max_budget_usd", 0.0);
	if (budget > 0)
		ctx->turn_cost_limit = ctx->usage_cost + budget;
	if (fyai_cmd_arg_bool(call, "include_partial_messages")) {
		srv.partial = true;
		ctx->text_observe = sdk_text;
		ctx->text_observe_data = &srv;
	}

	while (!srv.failed) {
		if (srv.head) {
			in = srv.head;
			srv.head = in->next;
			if (!srv.head)
				srv.tail = &srv.head;
			if (sdk_is_clear(in->text))
				sdk_clear(&srv);
			else
				sdk_turn(&srv, in);
			free(in->text);
			sdk_images_free(in->images);
			free(in->origin_json);
			free(in);
			continue;
		}
		if (jsonrpc_conn_closed(srv.conn))
			break;
		rc = fyai_event_loop_step(el, -1);
		fyai_error_check(ctx, rc >= 0, out,
				 "claude-sdk: the event loop stopped");
	}
	while (jsonrpc_conn_has_output(srv.conn))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
	ctx->cfg->exit_status = srv.last_error || srv.failed ? 1 : 0;
	rc = 0;
	goto done;
out:
	rc = -1;
done:
	ctx->turn_observe = NULL;
	ctx->turn_observe_data = NULL;
	ctx->agent_observe = NULL;
	ctx->agent_observe_data = NULL;
	ctx->text_observe = NULL;
	ctx->text_observe_data = NULL;
	ctx->turn_cost_limit = 0;
	ctx->tool_gate = NULL;
	ctx->tool_gate_data = NULL;
	/* The servers of the SDK reach it through this frame: stop them first. */
	if (srv.tunnels)
		fyai_mcp_cleanup(ctx);
	for (t = srv.tunnels; t; t = t->next)
		t->conn = NULL;
	while ((in = srv.head)) {
		srv.head = in->next;
		free(in->text);
		sdk_images_free(in->images);
		free(in->origin_json);
		free(in);
	}
	while ((p = srv.pending)) {
		srv.pending = p->next;
		sdk_pending_free(p);
	}
	free(srv.session_id);
	while ((d = srv.decisions)) {
		srv.decisions = d->next;
		sdk_decision_free(d);
	}
	while ((t = srv.tunnels)) {
		srv.tunnels = t->next;
		free(t->name);
		free(t);
	}
	sdk_hooks_free(&srv);
	sdk_rules_free(&srv.allow);
	sdk_rules_free(&srv.deny);
	free(srv.init_id);
	free(srv.result_text);
	jsonrpc_conn_destroy(srv.conn);
	return rc;
}
