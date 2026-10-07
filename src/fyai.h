/*
 * fyai.h - fyai runtime context and helper interfaces
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_H
#define FYAI_H

#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <limits.h>

#include <curl/curl.h>
#include <libfyaml.h>
#include <libfyaml/libfyaml-allocator.h>
#include <libfyaml/libfyaml-generic.h>
#include <libfypalette.h>

#include "utils.h"
#include "commands.h"
#include "fyai_auth.h"
#include "fyai_diag.h"

struct fyai_fenced_stream;	/* live progressive shell output (fyai_markdown.h) */
struct fyai_ui;
struct jsonrpc_conn;
struct fyai_display_output;
struct fyai_tool_job;
struct fyai_btw_run;
struct fyai_shell_session;
struct fyai_sink;
struct fyai_patch_display;	/* resolved patch presentation (fyai_tools.c) */

#define OPENAI_RESPONSES_URL "https://api.openai.com/v1/responses"
#define OPENAI_CHAT_COMPLETIONS_URL "https://api.openai.com/v1/chat/completions"
#define ANTHROPIC_MESSAGES_URL "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION "2023-06-01"

static inline fy_generic fyai_generic_or_null(fy_generic v)
{
	return fy_is_valid(v) ? v : fy_null;
}
#define DEFAULT_OPENAI_MODEL "gpt-5.4-mini"
#define DEFAULT_ANTHROPIC_MODEL "claude-sonnet-5"
/* Anthropic requires an explicit output-token cap on every request. */
#define DEFAULT_MAX_TOKENS 8192
#define DEFAULT_SYSTEM_PROMPT "You are a concise assistant."
#define DEFAULT_PARALLEL_TOOL_CALLS_PROMPT \
	"Independent tool calls may be issued together in one response and " \
	"will execute in parallel. Keep dependent or potentially conflicting " \
	"tool calls in separate responses."
/* increase the tool loop for what contemporary models output */
#define MAX_TOOL_LOOP_ITERATIONS 1000
#define DEFAULT_TEMPERATURE 0.0
/* Shell time limits in milliseconds. Zero disables either limit. */
#define DEFAULT_RETRY_MAX_ATTEMPTS 8
#define DEFAULT_RETRY_INITIAL_DELAY_MS 500
#define DEFAULT_RETRY_MAX_DELAY_MS 30000
#define DEFAULT_SHELL_TIMEOUT_MS 120000
#define DEFAULT_CATALOG_UPDATE_COMMAND "scrape-providers"
#define DEFAULT_CATALOG_UPDATE_TIMEOUT_MS 600000
#define DEFAULT_SHELL_MAX_TIMEOUT_MS 600000
/* Default and maximum read_file result sizes. Zero disables each limit. */
#define DEFAULT_READ_MAX_BYTES (256 * 1024)
#define DEFAULT_READ_HARD_MAX_BYTES (4 * 1024 * 1024)
/*
 * Shell output becomes prompt text for the same reason a read_file result
 * does, so it is bounded the same way. The unit is tokens because that is
 * what the budget it protects is measured in; bytes follow by the estimator's
 * bytes/4 rule. 16k tokens is a long build log, not a whole context window.
 */
#define DEFAULT_SHELL_MAX_OUTPUT_TOKENS 16384
#define DEFAULT_SHELL_SESSION_TIMEOUT_MS 900000
#define DEFAULT_SHELL_INPUT_POLL_MS 400
#define DEFAULT_SHELL_HARD_MAX_OUTPUT_TOKENS 200000
/* The bytes/4 rule the context estimator uses, in one place. */
#define FYAI_BYTES_PER_TOKEN 4
/* Default rendered rows of a tool result shown in the display view. */
#define DEFAULT_TOOL_PREVIEW_LINES 5
/* Rows of a running tool call that its tile keeps for scrolling back. */
#define DEFAULT_TOOL_HISTORY_LINES 40
/* Interactive history recap: -1 fills the screen, 0 is off, N is a count. */
#define DEFAULT_RECAP_EXCHANGES (-1)
#define DEFAULT_TOOL_UPDATE_INTERVAL_MS 33
/* Left indent applied to each rendered tool-output row (nests it under the
 * tool-call header), so the live loop and the history view match. */
#define FYAI_TOOL_OUTPUT_INDENT "    "
/* Align terminal content with its title. */
/* The rows that open reasoning in a stored assistant document. */
#define FYAI_REASONING_HEAD "> **\xf0\x9f\x92\xad reasoning**\n>\n"
#define FYAI_SESSION_MARGIN "  "
/* Work-pane defaults. */
#define DEFAULT_WORK_LAYOUT "auto"
#define DEFAULT_WORK_PANELS "auto"
#define DEFAULT_PAGE_LAYOUT "auto"
#define DEFAULT_WORK_POSITION "above-prompt"
/* Where an editor runs: a tile of the work pane, or the whole terminal. */
#define DEFAULT_EDITOR_MODE "pane"
#define DEFAULT_COMPLETION_MODE "tab"
#define DEFAULT_COMMAND_OUTPUT "transcript"
#define DEFAULT_COMMAND_BG "theme"
#define DEFAULT_COMMAND_BG_MIX 6
#define DEFAULT_TOOL_DISPLAY "inline"
#define DEFAULT_FOCUS_MARK "edge"
#define DEFAULT_INLINE_TERMINAL_ROWS 12
#define DEFAULT_FOCUS_BG "theme"
#define DEFAULT_FOCUS_BG_MIX 35
#define DEFAULT_WORK_ZOOM_ROWS "half"
#define DEFAULT_WORK_MIN_TILE_COLS 40
#define DEFAULT_WORK_HISTORY_ROWS 1000
#define DEFAULT_WORK_OPEN_DELAY_MS 500
#define DEFAULT_WORK_FRAME "none"
#define DEFAULT_WORK_CAP false
#define DEFAULT_TILE_FRAME "none"
/* Separate adjacent tile columns. */
#define DEFAULT_TILE_SEP " ┃ "
#define DEFAULT_WORK_CONTROLS "full"
/* Put the page on the alternate screen or in the scrollback. */
#define DEFAULT_SCREEN "fullscreen"
/* Mark shell commands and align continuation rows. */
#define FYAI_TOOL_MARKER "⎿  "
#define FYAI_TOOL_MARKER_PAD "   "
/* Minimum content width after reserving decoration columns. */
#define FYAI_MIN_RENDER_COLS 8
/* Display columns of both, for the layout that has to reserve them. */
#define FYAI_TOOL_MARKER_WIDTH 3
/* Default separators (markdown, themed by the renderer). Every separator is
 * empty: a blank line, and no rule between exchanges. */
#define DEFAULT_TURN_SEPARATOR ""
#define DEFAULT_TOOL_SEPARATOR ""
#define DEFAULT_SECTION_SEPARATOR ""
#define DEFAULT_TOOL_GROUP_FENCE 1
#define DEFAULT_USER_CARD_FENCE 1
/* Interactive prompt bubble: an empty prompt marker/top row keep the built-in
 * defaults; the bottom row is a {key} template reproducing the classic banner. */
#define DEFAULT_PROMPT_BOTTOM \
	" {model} · {provider} · {api}{effort}{summary}{temp}" \
	"{tokens}{cache}{cost}{isolation}{view}"
#define DEFAULT_PROMPT_TOP " {location}"
/* Streaming markdown render cadence / colour / theme defaults. */
#define DEFAULT_MARKDOWN_MODE "line"	/* oneshot | line | stream */
#define DEFAULT_MARKDOWN_UPDATE_INTERVAL_MS 50
#define DEFAULT_COLOR "auto"		/* auto | off | on */
/* markdown theme[:auto|dark|light]; a palette theme needs libfypalette. */
#define DEFAULT_THEME "ember:auto"
#define DEFAULT_TOOL_DETAIL "default"	/* none | brief | default | full */
/* Display reasoning/thinking model output (live stream + history view). */
#define DEFAULT_THINKING true
/*
 * Durable arena root schema version. The root contains the catalogue, HEAD,
 * branch mapping, and root ref log. Each branch contains its configuration,
 * conversation head, metadata, and branch ref log.
 *
 * Version 2 is not compatible with version 1 and no migration is attempted;
 * a root of any other version - including pre-container turn-shaped roots - is
 * rejected and the user re-inits. See doc/branching.md.
 */
#define FYAI_ROOT_VERSION 3

/* The branch an arena starts on, and the fallback when none is selected. */
#define FYAI_BRANCH_DEFAULT "main"

/* Maximum nesting depth of sub-agent branches below a top-level branch. */
#define DEFAULT_AGENT_MAX_BRANCH_DEPTH 8
#define DEFAULT_AGENT_SPAWN "exec"
#define DEFAULT_AGENT_MAX_TIMEOUT_MS 3600000
#define DEFAULT_AGENT_HANG_TIMEOUT_MS 600000

/*
 * What to do when a concurrent invocation advanced the same branch while this
 * one was working. "abort" keeps the safe behaviour - nothing is written and
 * nothing is lost - and is the default because silently reordering a
 * conversation is a decision the user should make.
 */
#define DEFAULT_BRANCH_ON_CONFLICT "abort"

enum fyai_api_mode {
	FYAI_API_RESPONSES,
	FYAI_API_CHAT_COMPLETIONS,
	FYAI_API_MESSAGES,
};

struct fypal_ctx;

struct fyai_cfg {
	int exit_status; /* Exit status of a successfully executed CLI child. */
	struct fy_allocator *allocator;
	struct fy_generic_builder *gb;	/* the builder for the configuration */
	enum fyai_api_mode api_mode;
	const char *api_url;
	const char *system_prompt;
	const char *parallel_tool_calls_prompt;
	const char *model;
	const char *api_key;
	enum fyai_auth_mode auth_mode;
	bool chatgpt_auth;
	/*
	 * The catalogue supplies this endpoint capability during resolution.
	 * It specifies if the provider accepts the built-in shell tool for the
	 * selected API grammar. Do not persist this derived value.
	 */
	bool shell_tool_supported;
	/* The selected endpoint declares a provider-hosted web search tool. */
	bool web_search_supported;
	/* The endpoint implements the OpenAI-specific /responses/compact route. */
	bool response_compaction_supported;
	bool model_explicit;
	/*
	 * Set when the key was supplied explicitly (--api-key or a config
	 * api_key env mapping); a mid-session /model switch keeps it. A key
	 * derived from the provider's <PROVIDER>_API_KEY env var is not
	 * explicit and is re-derived for the new provider.
	 */
	bool api_key_explicit;
	/*
	 * Where the credential comes from, as the transport names a source:
	 * "env:NAME", "secret:NAME", or "mem:cli" for --api-key. NULL when the
	 * provider default applies. The value is never kept here.
	 */
	const char *api_key_ref;
	bool api_key_auto;
	const char *provider;
	const char *prompt;
	const char *reasoning_effort;
	const char *reasoning_summary;
	const char *markdown_mode;	/* oneshot | line | stream */
	int markdown_update_interval_ms;
	int render_width;		/* runtime renderer width; 0 => terminal */
	const char *color;		/* auto | off | on */
	const char *theme;		/* canonical markdown theme selector */
	const char *theme_ground;	/* display/theme_ground: theme | terminal */
	const char *theme_variant;	/* resolved dark | light */
	const char *markdown_theme;	/* resolved libfymd4c theme name */
	/* The palette of a palette theme, or NULL. A renderer borrows it. */
	struct fypal_ctx *palette;
	/* Every palette made, so a renderer that outlives a theme change keeps
	 * the palette it borrowed; freed at cleanup. */
	struct fypal_ctx **palettes;
	size_t npalettes;
	/* What the current palette was made for, so a reload that changes
	 * none of it reuses the palette. */
	const char *palette_theme;
	const char *palette_variant;
	bool palette_color;
	const char *palette_ground;
	/* The result of fyai_terminal_probe(). The probe runs one time: a
	 * later probe would run while the UI reads the terminal, and the UI
	 * would take the replies. A copy of the configuration keeps the
	 * result and does not probe. */
	struct fypal_term terminal;
	bool terminal_probed;
	/* Keys typed during the probe, for the UI to read first. The
	 * configuration that ran the probe owns them; a copy does not free
	 * them. */
	char *terminal_input;
	size_t terminal_input_len;
	const char *markdown_rev_on[2];	/* reverse-card pair, [0] dark [1] light */
	const char *markdown_rev_off[2];
	const char *turn_separator;	/* history inter-turn break (markdown) */
	const char *tool_separator;	/* rendered before a tool result (markdown) */
	const char *section_separator;	/* reasoning -> answer break (live stream) */
	int tool_group_fence;		/* blank rows around a tool exchange */
	int user_card_fence;		/* blank rows after the user card */
	const char *prompt_marker;	/* interactive prompt marker (SGR ok) */
	const char *prompt_top;		/* REPL bubble top row template (SGR ok) */
	const char *prompt_bottom;	/* REPL bubble bottom {key} template (SGR ok) */
	const char *branch_view;
	const char *branch_preview;
	int branch_preview_size;
	int branch_preview_width;	/* negative percent, positive columns */
	const char *diagram_theme;
	const char *diagram_charset;
	const char *diagram_fit;
	int table_border;		/* 0 theme (default) | 1 grid | 2 none */
	int max_tool_iterations;
	int max_tokens;			/* output cap (required by Messages) */
	int top_logprobs;
	int tool_preview_lines;
	int tool_history_lines;		/* rows of a live tool tile to scroll back */
	int recap_exchanges;		/* interactive history recap exchanges */
	int tool_update_interval_ms;
	int retry_max_attempts;		/* provider attempts, 1 = no retry */
	int retry_initial_delay_ms;	/* first backoff delay */
	int retry_max_delay_ms;		/* ceiling on one backoff delay */
	int shell_timeout_ms;		/* default shell time limit (0 = none) */
	int shell_max_timeout_ms;	/* cap on a model-requested limit */
	int read_max_bytes;		/* default read_file cap (0 = none) */
	int read_hard_max_bytes;	/* cap on a model-requested size */
	int shell_max_output_tokens;	/* default shell output cap (0 = none) */
	int shell_hard_max_output_tokens; /* cap on a model-requested size */
	int shell_session_timeout_ms;	/* idle limit of a named session */
	const char *shell_tty_term;	/* terminal type exposed to PTY commands */
	int shell_input_poll_ms;	/* how often a session is asked if it waits */
	const char *session_margin;	/* left chrome of a terminal session */
	const char *focus_bg;		/* ground of the focused tile, "" = none */
	const char *focus_bg_checked;	/* the literal ground already reported */
	int focus_bg_mix;		/* percent of it mixed into a colour */
	const char *focus_mark;		/* wash | edge */
	/* Work-pane configuration. */
	const char *work_layout;	/* auto | columns | stack */
	const char *work_position;	/* above-prompt | below-prompt */
	const char *editor_mode;	/* pane | terminal */
	const char *completion_mode;	/* tab | auto */
	const char *command_output;	/* pane | transcript */
	const char *command_bg;		/* theme | none | #rrggbb */
	int command_bg_mix;		/* percent of the theme colour */
	const char *tool_display;	/* pane | inline */
	int inline_terminal_rows;	/* rows of an inline terminal */
	int work_columns;		/* columns when work_layout is columns */
	/* A side layout of the page document: auto takes it at the size the
	 * document names, on whatever the size, off never. A panel size of 0
	 * keeps the size of the document. */
	const char *work_panels;
	/* The layout of the page document by name, or auto for the first that
	 * the terminal is large enough for. */
	const char *page_layout;
	int work_panel_cols;
	int work_panel_rows;
	int work_min_tile_cols;		/* narrowest tile the auto grid makes */
	int work_history_rows;		/* rows a tile keeps to scroll back to */
	int work_open_delay_ms;		/* delay before a shell work band opens */
	int work_max_rows;		/* rows the pane may take (0 = uncapped) */
	const char *work_zoom_rows;	/* full | half | quarter */
	int work_zoom_fixed_rows;	/* direct row count, 0 for named policy */
	const char *work_frame;		/* chrome around the pane */
	bool work_cap;			/* the cap row above the pane */
	const char *tile_frame;		/* chrome under a tile's title row */
	const char *tile_sep;		/* rule between adjacent columns */
	const char *work_controls;	/* none | zoom | full */
	const char *page_path;		/* display/page: a page document file */
	const char *screen;		/* display/screen: inline | fullscreen */
	bool agent_pty;			/* this sub-agent has a terminal */
	bool shell_tty;			/* run a shell call on a terminal by default */
	const char *shell_shell;	/* the shell a call runs under; empty = /bin/sh */
	/* The program that writes a catalogue, and its time limit. */
	const char *catalog_update_command;
	unsigned int catalog_update_timeout_ms;
	fy_generic catalog_update_credentials;	/* variable names */
	bool shell_login;		/* run a shell call under a login shell */
	int shell_tty_rows;		/* PTY rows (0 = follow the terminal) */
	int shell_tty_cols;		/* PTY columns (0 = follow the terminal) */
	int agent_timeout_ms;		/* sub-agent time limit (0 = none) */
	int agent_max_timeout_ms;	/* bound on a model-asked limit (0 = none) */
	int agent_hang_timeout_ms;	/* extra time after the advisory limit */
	bool stdin_consumed;		/* the prompt was read from standard input */
	const char *agent_transport_isolation;	/* none, auto, level-a or level-b */
	bool agent_timeout_kill;	/* terminate an agent after the extra time */
	int agent_max_branch_depth;	/* nesting cap for sub-agent branches */
	const char *agent_spawn;	/* exec or fork: how a sub-agent child starts */
	int agent_max_live_agents;
	const char *tool_detail;
	bool transcript_system;
	float temperature;
	bool enable_tools;
	/* Opt in to the selected provider's native web search facility. */
	bool web_search;
	bool parallel_tool_calls;
	bool enable_builtin_shell;
	bool enable_sandbox;	/* Landlock-confine shell tool sub-executions */
	fy_generic sandbox;	/* policy in force: mapping (allow/deny/network), or invalid */
	bool sandbox_lockdown;	/* the policy is the lockdown profile */
	bool interactive;
	int debug;
	bool pretty;
	bool markdown;
	bool thinking;
	bool cache_info;
	bool stats;
	bool stream;
	bool wire_logging;
	bool stream_logging;
	bool conversation_logging;
	bool mcp_logging;
	bool transport_logging;
	bool whitewash_api_keys;
	bool logprobs;
	/*
	 * Record per-token extents {text, pos, lp} from streamed responses in
	 * turn metadata. Requests logprobs from providers that support them;
	 * falls back to per-delta {text, pos} chunk extents elsewhere
	 * (Anthropic Messages, reasoning models).
	 */
	bool token_extents;
	bool no_obfuscation;
	/*
	 * Chain via previous_response_id (Responses API only): config-only,
	 * no CLI flag and default off. A stale/evicted response automatically
	 * falls back to replaying the canonical local turn chain. Enable with
	 * `config set response_chain true` / `--set response_chain=true`.
	 */
	bool response_chain;
	/*
	 * Skip the api_key requirement and the Authorization/x-api-key
	 * header, for local no-auth model servers (Ollama, llama.cpp's
	 * llama-server, vLLM, ...) speaking the Chat Completions wire
	 * format. Config-only, no CLI flag - `config set no_auth true` /
	 * `--set no_auth=true`.
	 */
	bool no_auth;
	bool new_conversation;
	/* Active branch selection and whether the user selected it explicitly. */
	char *branch;
	bool branch_explicit;
	/*
	 * Start this interactive session on a branch of its own, named when
	 * storage opens. An invocation that names no branch does not continue
	 * the conversation the last one left.
	 */
	bool fresh_session;
	/* Owned branch name for a restart after the session closes. */
	char *reload_branch;
	char *reload_config;
	char *reload_session;
	char *reload_key;
	char *reload_arena;
	fy_generic reload_state;	/* exec handoff, held by the config builder */
	/* Read-only root selection, resolved when the arena opens. */
	char *root_spec;
	fy_generic_value root_ref;
	bool root_pinned;
	/* Policy for a concurrent change to the active branch. */
	const char *branch_on_conflict;
	/*
	 * Stack an in-memory builder over the durable arena so every config and
	 * state write this session is ephemeral (never published to the arena).
	 */
	bool transient;
	/* True in a sub-agent child. */
	int transport_ctl_fd;		/* fyai transport: the control channel */
	bool agent_child;
	/* The parent limits a forked tool job. */
	bool tool_child;
	/* The command line of this process, kept to run it again in a view. */
	char *const *argv;
	/* Serve the agent protocol on standard input and output. */
	bool agent_rpc;
	/* An executed tool child: serve the tool channel on fds 3 and 4. */
	bool tool_exec;
	/* MCP (Model Context Protocol) server settings. */
	bool mcp_enabled;
	/* Wait for all MCP servers before the first model step. */
	bool mcp_startup_wait;
	const char *mcp_endpoint;		/* server URL or empty */
	const char *mcp_auth_token;	/* env/secret indirection (like api_key) */
	bool mcp_auth_token_auto;
	const char *mcp_protocol_version;
	fy_generic mcp_servers;		/* named server mapping (mapping generic) */
	int mcp_timeout;			/* seconds (default 30) */

	const char *arena_dir;
	/*
	 * Catalogue of the selected branch, internalized into gb (fy_invalid
	 * when the branch carries none - the embedded snapshot is the
	 * fallback). catalog_src is the arena value it was copied from.
	 */
	fy_generic catalog;
	fy_generic_value catalog_src;
	/*
	 * The single configuration source: one merged document (arena config
	 * as base - the user file is bootstrap-only when no arena config
	 * exists - then --config, then --set deltas on top). The struct
	 * fields below are a derived cache filled by one apply_config pass;
	 * `config effective` emits this document verbatim. Catalog-derived
	 * values (endpoint, provider, max_tokens) are never folded in - they
	 * are re-derived read-only from the catalogue at resolve time.
	 */
	fy_generic config_doc;
	/* Count configuration apply passes. */
	unsigned int config_generation;
	/* Pre-supplied answers for the ask_user tool, consumed in order
	 * (batch/non-interactive use). */
	const char *answers[10];	/* maximum 10 answers */
	size_t answer_count;

	/*
	 * Repeatable --set/--get/--delete config operations, applied in order
	 * once storage is open. --set folds into this run before model
	 * resolution and persists (unless transient); --delete/--get run at
	 * storage time. op is 's'/'g'/'d'.
	 */
	struct fyai_config_op {
		char op;
		const char *key;
		const char *value;
		bool persistent;	/* commit to the stored arena config */
		bool command;		/* explicit --set/--get/--delete */
	} config_ops[32];
	size_t config_op_count;

	/* the info about the command */
	struct fyai_cmd_info cmd;

	/* auth state in cfg builder */
	const char *auth_state_dir;

	/*
	 * Collected diagnostics. Lives here rather than on the context because
	 * it has to outlive it: option parsing and the verb argument hooks run
	 * before fyai_run() declares a context, and raise a third of the
	 * diagnostics in the tree.
	 */
	/*
	 * The session layer: settings that this session changed and does not
	 * store. It goes on top of the stored configuration, --config, and
	 * --set, and lives in @gb.
	 */
	fy_generic config_session;

	struct fyai_diag diag;
};

/* The properties of what @cfg runs, or NULL before it is chosen. */
const struct fyai_verb *fyai_cfg_verb(struct fyai_cfg *cfg);

static inline bool
fyai_cfg_no_requests(struct fyai_cfg *cfg)
{
	const struct fyai_verb *v = fyai_cfg_verb(cfg);
	return !v || (v->flags & FYAIVF_NO_REQUESTS);
}

static inline bool
fyai_cfg_makes_requests(struct fyai_cfg *cfg)
{
	return !fyai_cfg_no_requests(cfg);
}

static inline bool
fyai_cfg_no_storage(struct fyai_cfg *cfg)
{
	const struct fyai_verb *v = fyai_cfg_verb(cfg);
	return !v || (v->flags & FYAIVF_NO_STORAGE);
}

/* The verb opens the arena that exists and never creates one. */
static inline bool
fyai_cfg_storage_optional(struct fyai_cfg *cfg)
{
	const struct fyai_verb *v = fyai_cfg_verb(cfg);
	return v && (v->flags & FYAIVF_STORAGE_OPTIONAL);
}

static inline bool
fyai_cfg_uses_storage(struct fyai_cfg *cfg)
{
	return !fyai_cfg_no_storage(cfg);
}

struct fyai_mcp_ctx;
struct fyai_cmd_call;
struct fyai_config_edit_request;
struct fyai_catalog_update_request;

struct fyai_event_loop;
struct fyai_event_source;

/* Token and cost totals of the usage that some turns carry. */
struct fyai_usage_sum {
	long long input;
	long long cached;
	long long cache_write;
	long long output;
	long long reasoning;
	long long total;
	double cost;
	double cost_est;	/* the part of @cost that is estimated */
	int calls;
};

struct fyai_ctx {
	struct fyai_cfg *cfg;
	struct fy_allocator *durable_allocator;
	struct fy_generic_builder *durable_gb;
	/*
	 * The working state/config builder: durable_gb normally, or an in-memory
	 * builder stacked over it when cfg->transient is set. All canonical
	 * config and turn state is built through ctx->gb; in transient mode the
	 * refs-publish is skipped so nothing reaches the durable arena. When it
	 * differs from durable_gb, @overlay_allocator backs it and both are
	 * released in fyai_close_storage.
	 */
	struct fy_generic_builder *gb;
	struct fy_allocator *overlay_allocator;
	struct fy_allocator *transient_allocator;
	struct fy_generic_builder *transient_gb;
	/* Release idle-operation scratch storage on the next loop iteration. */
	bool transient_autorelease;
	CURL *curl;
	/* Channel to the credential transport; NULL when this process talks to
	 * the provider itself. See fyai_xfer.h. */
	struct fyai_tclient *tclient;
	int transport_ctl;		/* control channel; -1 if none */
	pid_t transport_pid;
	bool transport_owner;		/* this process started the transport */
	/* This process holds the primary control connection and states the profiles. */
	bool transport_primary;
	struct fyai_event_source *transport_src;	/* drains the control channel */
	uint64_t transport_exec;	/* the execution the transport knows us as */
	long long transport_seq;
	char transport_why[160];	/* the cause of a failed control request */
	/* Profile names that this execution has stated and been granted. */
	char transport_names[64][64];
	unsigned int transport_nnames;
	unsigned int transport_gen;	/* the configuration the supervisor last stated */
	bool transport_stated;
	int transport_envfd;		/* read end of a credential grant; <= 2: none */
	/* The model transfer in progress. */
	const char *xfer_body;
	const char *xfer_url;		/* NULL: the endpoint of the configuration */
	const char *xfer_profile;	/* NULL: "model" */
	long xfer_status;		/* of the last transfer that finished */
	long xfer_retry_after;
	char xfer_error[256];
	/* Per-invocation curl multi state. */
	struct fyai_curl_state *curl_state;

	/* The one application event loop. */
	struct fyai_event_loop *el;
	struct fyai_event_loop *event_loop_pool;
	struct fyai_event_source *event_source_pool;
	struct fyai_event_source *signal_src[4];
	sigset_t signal_mask;
	bool signal_mask_valid;
	/* Diagnostic output descriptor. */
	int dump_fd;
	struct fyai_ui *ui;
	struct fyai_browser *browser;
	struct fyai_agents *agents;
	long long agent_execution, agent_parent;
	struct fyai_config_edit_request *config_edit;
	struct fyai_catalog_update_request *catalog_update;
	struct fyai_cmd_call *cmd_call;	/* the active async command */
	/* The SIGINT handler can set this value. */
	volatile sig_atomic_t interrupt_pending;
	/* Count SIGINT edges while interrupt_pending remains set. */
	volatile sig_atomic_t interrupt_seq;
	sig_atomic_t interrupt_seen;
	bool terminate_pending;
	/* Do not apply the context guard to a compaction request. */
	bool compacting;
	/* Output allowance after the context check. Zero uses the configuration. */
	long long context_max_tokens;
	fy_generic tools;
	/* The built-in tool specification in the configuration builder. */
	fy_generic tools_spec;
	unsigned int tools_spec_generation;
	bool tools_spec_agent_child;
	fy_generic last_message;
	fy_generic arena_config;	/* the active branch's config, or fy_invalid */
	fy_generic arena_catalog;	/* catalogue of the branch, or fy_invalid */
	/* Active branch state and its next ref-log predecessor. */
	char *branch;
	/* Stored HEAD, which can differ from the active branch. */
	char *head_branch;
	/*
	 * The fresh session that no publish stored yet, or NULL. While its
	 * conversation holds no exchange, a publish on it stores nothing and
	 * its configuration stays in this context.
	 */
	char *session_unstored;
	/* Durable branch for a sub-agent conversation. */
	char *agent_branch;
	/* The spawn state an executed sub-agent child received, as JSON. */
	char *agent_spawn_json;
	char *tool_submit_error;
	fy_generic arena_branches;
	fy_generic branch_prev;
	fy_generic branch_desc;		/* free-text purpose of this branch */
	/*
	 * Label for the next publish on this branch, and the old name when
	 * that label is a rename. Both are consumed and cleared by the
	 * publish, so a label cannot leak into a later, unrelated entry.
	 */
	const char *branch_op;
	const char *branch_op_from;
	fy_generic branch_agent;	/* sub-agent provenance for this branch */
	/*
	 * Store that the next publish starts from in place of the store of
	 * branch_prev, or fy_invalid. The publish consumes it.
	 */
	fy_generic branch_store;
	/* The project state that the next entry records, or fy_invalid. */
	fy_generic project_state;
	/*
	 * The groups of tool calls of this session that changed the project and showed it:
	 * a sequence of {before, after} project states, the newest last. `undo` takes it back.
	 */
	fy_generic tool_changes;
	/* The private storage of the project states of a run in a view exists. */
	bool tool_diff_storage;
	/*
	 * The turn in flight shows what each group of tool calls changed, so a patch
	 * does not display its own diff. Set from the start of the turn until it ends,
	 * and cleared when the project states can no longer be taken.
	 */
	bool tool_diff_tracking;
	uint64_t refs_head;
	struct curl_slist *headers;
	char *auth_header;
	char *user_agent;
	fy_generic mcp_tools;
	struct fyai_mcp_ctx *mcp;
	bool mcp_stopping;
	struct fyai_credentials auth;
	struct fy_generic_builder *auth_gb;
	bool auth_retry_done;
	bool stdout_tty;			/* stdout is a terminal (cached) */
	/* The size of the real terminal, recorded by the parent and kept up to
	 * date by SIGWINCH. A forked tool child inherits it, because it calls
	 * setsid() and can no longer ask the kernel itself. 0 = unknown. */
	int tty_rows;
	int tty_cols;
	void *tty_session;		/* the PTY session running in this process */
	struct fyai_tool_job *tool_jobs;	/* live jobs, for a resize */
	struct fyai_btw_run *btw_runs;	/* side questions in this session */
	/* Named terminal sessions, each one a process of its own. The view of
	 * a session lives here and so outlives the process that drove it. */
	struct fyai_shell_session *shell_sessions;
	/* The one owner of work-pane geometry, focus, and zoom. */
	struct fyai_workpane_manager *workpane;
	struct fyai_wait *waits;	/* named waits, live for this run */
	/* Events queued for model turns in arrival order. */
	struct fyai_pending_event *events;
	struct fyai_pending_event **events_tail;
	struct fyai_event_source *winch_src;
	bool tool_output_displayed;
	/* Set once a tool row has been drawn live, so the next one gets a
	 * blank line ahead of it */
	/* The sole progressive transcript document for the active user or
	 * assistant output. Owned by this context, never by a signal handler. */
	struct fyai_display_output *display_output;
	/* The one rendering component. Every byte the user sees goes here. */
	struct fyai_sink *sink;
	struct fyai_fenced_stream *shell_stream; /* live progressive shell output */
	/* Resolved patch display data, indexed by tool-call ID. */
	struct fyai_patch_display *patch_views;
	/* Resolved display data for the active patch call. */
	char *patch_display;
	/* Forked tool control channel. */
	struct jsonrpc_conn *tool_rpc;
	/* Set inside a forked tool sub-execution once the environment has been
	 * sanitized and the sandbox applied, so inner steps (the shell tool's
	 * own fork) do not re-derive and re-apply the confinement. */
	bool sandbox_applied;
	/* Index of the next pre-supplied --answer to hand to ask_user. */
	size_t answer_next;
	/* Set when ask_user needs an answer but none can be obtained
	 * (non-interactive stdin with no --answer left); aborts the run. */
	bool ask_abort;
	/* Accumulated token usage across all model calls in this run. */
	long long usage_input;
	long long usage_cached;
	long long usage_cache_write;
	long long usage_output;
	long long usage_reasoning;
	long long usage_total;
	double usage_cost;
	/* The part of usage_cost that is estimated from the catalogue prices. */
	double usage_cost_est;
	int usage_calls;
	/*
	 * What the sub-agents of the branch used, from their own branches.
	 * Made again when the branch table or the head changes; @usage_agents_root
	 * is the branch table that it was made from.
	 */
	struct fyai_usage_sum usage_agents;
	int usage_agent_count;
	fy_generic usage_agents_root;
	/* The turn that the counters above total; see fyai_usage_sync(). */
	fy_generic usage_head;
	/*
	 * The cost of a change of model: the next request has no cache on the
	 * new model. @switch_prefix tokens cost @switch_fresh there, and
	 * @switch_stay on the model that was left. Pending until a call ends.
	 */
	bool switch_pending;
	long long switch_prefix;
	double switch_fresh;
	double switch_stay;
	/* Last model call's usage (ground truth for context fill). */
	long long last_call_input;
	long long last_call_output;
	long long last_call_total;
	/* Token extents collected by the last streamed call (fy_invalid when
	 * none); consumed when the assistant response is appended to a turn. */
	fy_generic last_token_extents;
	/* Fail-soft latch: set when a provider rejected the logprobs params we
	 * injected for token_extents, so the session stops asking. */
	bool token_extents_off;
	/* The last Responses request failed because previous_response_id could
	 * not be resolved. The model loop retries that step from local history. */
	bool response_chain_linked;
	bool response_chain_miss;
};

static inline bool fyai_interrupt_pending(const struct fyai_ctx *ctx)
{
	return ctx && ctx->interrupt_pending;
}


static inline bool fyai_interrupt_check(struct fyai_ctx *ctx)
{
	bool pending;

	if (!ctx)
		return false;
	pending = ctx->interrupt_pending;
	ctx->interrupt_pending = false;
	return pending;
}

int
fyai_setup(struct fyai_ctx *ctx, struct fyai_cfg *in_cfg);

void
fyai_cleanup(struct fyai_ctx *ctx);

int
fyai_execute(struct fyai_ctx *ctx);

void
fyai_print_usage_stats(struct fyai_ctx *ctx);

int
fyai_mkdir_p(const char *path);

int fyai_prompt(struct fyai_ctx *ctx);

const char *fyai_api_to_string(enum fyai_api_mode api);

void fyai_cleanup_transient_builder(struct fyai_ctx *ctx);
int fyai_setup_transient_builder(struct fyai_ctx *ctx);
/*
 * Return scratch storage for the current operation. Create temporary storage
 * when no active turn owns it.
 */
struct fy_generic_builder *fyai_ctx_transient_gb(struct fyai_ctx *ctx);

/*
 * Run one complete tool-use loop on @turn; returns the final turn (or
 * fy_invalid on failure). Exposed for /compact's one-off summary request.
 * On an interrupted/failed run a diagnostic is attached to the returned
 * generic (FYGIF_DIAG indirect); when steps completed before the failure the
 * wrapped value is the partial turn, otherwise fy_invalid.
 */
fy_generic fyai_run_turn(struct fyai_ctx *ctx, fy_generic turn);

/* Queue owned @text for the event-loop owner to submit between turns. */
int fyai_event_inject(struct fyai_ctx *ctx, char *text);
/*
 * What a queued event reports for. A wait event names its owner: the
 * report reaches the model after the poll that queued it, and the wait
 * may be gone by then. The owner pairs the report with the liveness of
 * the job or session it came from: a stale wait is dropped instead of
 * starting a turn for a program that has ended.
 */
enum fyai_event_owner_kind {
	FYAI_EVENT_OWNER_NONE,		/* owned by nobody: always delivered */
	FYAI_EVENT_OWNER_AGENT,		/* branch of a live sub-agent job */
	FYAI_EVENT_OWNER_SESSION,	/* name of a live shell session */
};
/* Queue owned @text with its owner; the caller gives away @owner. */
int fyai_event_inject_owned(struct fyai_ctx *ctx, char *text,
			    enum fyai_event_owner_kind owner_kind,
			    char *owner);
/* Take the oldest queued event, or NULL. The caller owns it. */
char *fyai_event_take(struct fyai_ctx *ctx);
bool fyai_event_queued(const struct fyai_ctx *ctx);
/* Take the oldest event with a live owner, dropping stale waits first. */
char *fyai_event_take_live(struct fyai_ctx *ctx);
void fyai_events_release(struct fyai_ctx *ctx);
/* Drop the queued wait events of one settled owner. */
void fyai_events_drop_agent(struct fyai_ctx *ctx, const char *branch);
void fyai_events_drop_session(struct fyai_ctx *ctx, const char *name);

/* Wrap @value (possibly fy_invalid) with a diagnostic message. */
fy_generic fyai_with_diag(struct fy_generic_builder *gb, fy_generic value,
			  const char *msg);

/* Print an attached diagnostic to stderr; returns the unwrapped value. */
fy_generic fyai_report_diag(struct fyai_ctx *ctx, fy_generic v);

/*
 * (Re)build the per-session request state derived from cfg: auth header,
 * header list, endpoint URL and the tools document. Requires an active
 * transient builder. Used at setup and after a mid-session /model switch.
 */
int fyai_request_state_apply(struct fyai_ctx *ctx);

/*
 * Create the model curl handle and apply its base options. Do not reuse a curl
 * connection cache after fork.
 */
int fyai_curl_easy_reinit(struct fyai_ctx *ctx);

/*
 * True if a model request can be authenticated: this image holds a key, a
 * ChatGPT login, or needs none, or the key is at the credential transport,
 * which this image cannot see.
 */
static inline bool fyai_credential_available(const struct fyai_ctx *ctx)
{
	const struct fyai_cfg *cfg = ctx->cfg;

	return (cfg->api_key && *cfg->api_key) || cfg->chatgpt_auth ||
	       cfg->no_auth || ctx->tclient;
}

#endif
