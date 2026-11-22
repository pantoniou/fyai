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
#include "fyai_askq.h"
#include "fyai_branch_state.h"
#include "fyai_agent_state.h"
#include "fyai_jobs.h"
#include "fyai_out.h"
#include "fyai_screen.h"
#include "fyai_uistores.h"
#include "fyai_link.h"
#include "fyai_turnstate.h"
#include "fyai_usage.h"
#include "fyai_scope.h"
#include "fyai_skills.h"
#include "fyai_tty.h"
#include "fyai_toolspec.h"
#include "fyai_waitq.h"
#include "fyai_mcp_state.h"
#include "fyai_transport_state.h"
#include <libfyaml.h>
#include <libfyaml/libfyaml-allocator.h>
#include <libfyaml/libfyaml-generic.h>
#include <libfypalette.h>

#include "fyai_cover.h"
#include "utils.h"
#include "commands.h"
#include "fyai_auth.h"
#include "fyai_diag.h"
#include "fyai_loop.h"
#include "fyai_transient.h"
#include "fyai_arena.h"
#include "fyai_media.h"

struct fyai_fenced_stream;	/* live progressive shell output (fyai_markdown.h) */
struct fyai_ui;
struct fyai_app;
struct fyai_uimodel;
struct fyai_uilog;
struct fyai_uiwork;
struct fyai_uitranscript;
struct fyai_uidraw;
struct fyai_ask_entry;
struct jsonrpc_conn;
struct fyai_display_output;
struct fyai_tool_job;
struct fyai_btw_run;
struct fyai_shell_session;
struct fyai_sink;
struct fyai_turn_run;
struct fyai_tool_gate_ops;
struct fyai_gate_relay;

/* What stopped a turn before the model ended it. */
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
#define DEFAULT_FLEET_SCAN_DEPTH 4
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
/*
 * An attachment is sent whole and counted against the context of every
 * request that carries it, so its size and its number in one message are
 * both bounded.
 */
#define DEFAULT_MEDIA_MAX_BYTES (4 * 1024 * 1024)
#define DEFAULT_MEDIA_MAX_PER_MESSAGE 8
/* How long a clipboard helper may run before the read is refused. */
#define DEFAULT_MEDIA_CLIPBOARD_TIMEOUT_MS 5000
/* The bytes/4 rule the context estimator uses, in one place. */
#define FYAI_BYTES_PER_TOKEN 4
/* Default rendered rows of a tool result shown in the display view. */
#define DEFAULT_TOOL_PREVIEW_LINES 5
#define DEFAULT_TILE_LIVE_ROWS 5
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
#define DEFAULT_CHORD_TIMEOUT_MS 1000
#define DEFAULT_COMMAND_OUTPUT "transcript"
#define DEFAULT_NOTICE_OUTPUT "transcript"
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
#define DEFAULT_PROMPT_TOP " {fleet}{location}{sandbox}"
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

/* The number of grammars. Keep it outside the enum: a switch need not name it. */
#define FYAI_API_COUNT (FYAI_API_MESSAGES + 1)

struct fypal_ctx;

/* How long the processes of a sandboxed tool live (`sandbox_scope`). */
enum fyai_scope_kind {
	FYAI_SCOPE_EXCHANGE,		/* until the exchange ends: the default */
	FYAI_SCOPE_SESSION,		/* until the interactive session ends */
	FYAI_SCOPE_CALL,		/* until the call ends */
};

/*
 * The Responses chain of a request. Each request sets UNLINKED or LINKED when
 * it is submitted. A LINKED request that the provider refuses because
 * previous_response_id cannot be resolved becomes MISSED, and the model loop
 * retries that step from local history.
 */

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
	/*
	 * The tool type the endpoint declares that search with, from the
	 * catalogue, or NULL for the default of the grammar. Not persisted.
	 */
	const char *web_search_type;
	/* An unsupported-search notice is emitted once per invocation. */
	bool web_search_notice_emitted;
	/* The endpoint implements the OpenAI-specific /responses/compact route. */
	bool response_compaction_supported;
	/* Derived from the selected model's modalities; not persisted. */
	bool image_input_supported;
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
	unsigned markdown_rev_pending;	/* bit i: pair i is not probed yet */
	const char *turn_separator;	/* history inter-turn break (markdown) */
	const char *tool_separator;	/* rendered before a tool result (markdown) */
	const char *section_separator;	/* reasoning -> answer break (live stream) */
	int tool_group_fence;		/* blank rows around a tool exchange */
	int user_card_fence;		/* blank rows after the user card */
	const char *prompt_marker;	/* interactive prompt marker (SGR ok) */
	const char *prompt_busy;	/* glyph of the busy prompt marker; "" => theme */
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
	bool durable;			/* checkpoint turn after each round */
	int max_tokens;			/* output cap (required by Messages) */
	int top_logprobs;
	int tool_preview_lines;
	int tile_live_rows;	/* live rows of a tile in a shared pane */
	int tool_history_lines;		/* rows of a live tool tile to scroll back */
	int recap_exchanges;		/* interactive history recap exchanges */
	int tool_update_interval_ms;
	int idle_poll_ms;
	int retry_max_attempts;		/* provider attempts, 1 = no retry */
	int retry_initial_delay_ms;	/* first backoff delay */
	int retry_max_delay_ms;		/* ceiling on one backoff delay */
	int shell_timeout_ms;		/* default shell time limit (0 = none) */
	int shell_max_timeout_ms;	/* cap on a model-requested limit */
	int read_max_bytes;		/* default read_file cap (0 = none) */
	int read_hard_max_bytes;	/* cap on a model-requested size */
	/* Attachments a message may carry, and the size of one (0 = no bound). */
	long long media_max_bytes;
	int media_max_per_message;
	/* The program that reads an image out of the clipboard, or NULL. */
	const char *media_clipboard_command;
	int media_clipboard_timeout_ms;
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
	int chord_timeout_ms;		/* wait of a chord for its next key */
	const char *command_output;	/* pane | transcript */
	const char *notice_output;	/* pane | transcript */
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
	double temperature;
	bool enable_tools;
	/* Opt in to the selected provider's native web search facility. */
	bool web_search;
	bool parallel_tool_calls;
	bool enable_builtin_shell;
	/* The ceiling of model_access: an enum fyai_model_access, or -1 for
	 * none, which offers no settings tool. */
	int model_access;
	bool enable_sandbox;	/* Landlock-confine shell tool sub-executions */
	fy_generic sandbox;	/* policy in force: mapping (allow/deny/network), or invalid */
	const char *sandbox_profile;	/* profile that made the policy; NULL: `sandbox` */
	const char *sandbox_reported;	/* what the last report of open egress was for */
	const char *sandbox_on_failure;	/* a sandbox that cannot be set up: fail or warn */
	const char *sandbox_cover;	/* how a tool hides the arena: auto, userns, bwrap, none */
	long long sandbox_grace_ms;	/* time that a process of a scope has to stop after SIGTERM */
	const char *sandbox_scope;	/* how long its processes live: exchange, session, call */
	enum fyai_scope_kind sandbox_scope_kind; /* the same, derived from sandbox_scope */
	const char *cast_path;		/* --cast: the file that records the screen, or NULL */
	bool cast_compat;		/* --cast-compat: write plain asciicast, nothing of ours */
	bool interactive;
	int debug;
	bool pretty;
	bool markdown;
	bool markdown_unshown;		/* a worker renders for a client that shows the source */
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
	 * Keep the text of the input of the last request and make only the
	 * text of the new turns for the next one (`request_cache`).
	 */
	bool request_cache;
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
	const char *branch;
	bool branch_explicit;
	/*
	 * Start this interactive session on a branch of its own, named when
	 * storage opens. An invocation that names no branch does not continue
	 * the conversation the last one left.
	 */
	bool fresh_session;
	/*
	 * Name of the fresh session, interned in the configuration builder, else
	 * storage names it. A session that a program names takes the
	 * configuration of the branch HEAD names, not of the session that ran
	 * last in its directory.
	 */
	const char *fresh_session_name;
	/*
	 * Name of a branch that the fresh session starts from, interned in the
	 * configuration builder: its conversation and configuration, as a fork
	 * of it.
	 */
	const char *fresh_session_from;
	/* Owned branch name for a restart after the session closes. */
	char *reload_branch;
	/*
	 * Owned live session branch to exec-attach to after cleanup.
	 */
	char *attach_branch;
	/*
	 * Optional `fleet attach` parameters paired with attach_branch:
	 * owned target project dir (NULL = current project), read-only watch
	 * flag, and owned return session name for `/root` (or NULL).
	 */
	char *attach_dir;
	bool attach_watch;
	char *attach_return;
	/*
	 * Owned follow-up target to exec after cleanup, or NULL: "" starts a
	 * new session, "<branch>" resumes a branch, "jump:NAME RETURN" jumps
	 * to fleet session NAME with return target, and "root:" returns to
	 * $FYAI_FLEET_RETURN.
	 */
	char *next_session;
	bool reload_worker;	/* the restart runs the worker of a split client */
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
	/*
	 * `serve --daemon`: what the server in the background reported. This
	 * process presents it and ends.
	 */
	const char *serve_daemon;
	/* --remote: the rest of the command line runs at a server (`remote run`). */
	bool remote;
	/* --split: the interactive session runs at a worker that is a child. */
	bool split;
	/* --bootstrap: user confirmed creating a missing project directory. */
	bool bootstrap_confirmed;
	/*
	 * --fleet-parent PROJECT:BRANCH and --fleet-task ID: originating fleet
	 * session and task id persisted as branch lineage on first publish
	 * (doc/fyai-fleet-sdd.md, section 9).
	 */
	const char *fleet_parent;
	long long fleet_task;
	/* Cached fleet tool availability: 0 unknown, 1 enabled, -1 disabled. */
	int fleet_tool;
	const char *remote_socket;	/* the socket of --remote TARGET, or NULL */
	/* The global options and the words to run there, without the above. */
	fy_generic remote_args;
	size_t remote_globals;		/* how many of them are global options */
	/* The arguments of a `resume` at the server, which is interactive. */
	fy_generic remote_resume;
	/* The command line of --remote has a verb or a prompt. */
	bool remote_words;
	/* --project: the directory to work in; with --remote, the server's. */
	const char *project;
	/* --sink-frames: the sink sends each output as a frame on standard output. */
	bool sink_frames;
	/* Serve the agent protocol on standard input and output. */
	bool agent_rpc;
	bool acp_worker;
	/* Standard output carries protocol frames only. */
	bool protocol_stdout;
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

	/* Invocation-local filesystem view paths; never stored in configuration. */
	const char *view_project;
	const char *view_scratch;
	/*
	 * The arena of an agent runtime in a view: a detached mount that only
	 * this descriptor reaches, and its /proc/self/fd path. -1 and NULL
	 * when absent. The descriptor is close-on-exec; only an execution of
	 * this program receives it.
	 */
	int view_arena_fd;
	const char *view_arena;
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
	/* Images that the first message of this invocation carries. */
	const char *images[8];
	size_t image_count;

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
	 * Base URL of the token refresh endpoints. NULL, the only value that
	 * the program sets, means the issuer of FYAI_AUTH_ISSUER. A unit test
	 * sets it to reach a server of its own; no setting or environment
	 * variable can, because the refresh token goes to this URL.
	 */
	const char *auth_refresh_base;

	/*
	 * The session layer: settings that this session changed and does not
	 * store. It goes on top of the stored configuration, --config, and
	 * --set, and lives in @gb.
	 */
	fy_generic config_session;
	fy_generic config_cli;		/* the --set and -m layers alone */

	/*
	 * Collected diagnostics: a dependency, owned by struct fyai_app, which
	 * outlives the context. Option parsing and the verb argument hooks run
	 * before fyai_run() declares a context and raise a third of the
	 * diagnostics in the tree. NULL: a diagnostic is written at once.
	 */
	struct fyai_diag *diag;
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

/*
 * The run reads the model that the catalogue resolves: a verb that sends
 * requests, or one that shows or replays the resolved model. Resolution
 * parses the catalogue, so any other verb starts without it.
 */
static inline bool
fyai_cfg_needs_model(struct fyai_cfg *cfg)
{
	const struct fyai_verb *v = fyai_cfg_verb(cfg);

	return fyai_cfg_makes_requests(cfg) ||
	       (v && (v->flags & FYAIVF_MODEL_STATE));
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
struct fyai_orphan;
struct fyai_home;


struct fyai_ask_pending;
struct fyai_skill_dir;

struct fyai_request_cache;
struct fyai_link;

struct fyai_ctx {
	struct fyai_cfg *cfg;
	/* The arena component, owned by struct fyai_app (src/store/fyai_arena.h). */
	struct fyai_arena *arena;
	/* The transient builder component, owned by struct fyai_app. */
	struct fyai_transient *transient;
	/* The model connection component, owned by struct fyai_app. */
	struct fyai_link *link;
	/* The credential transport component, owned by struct fyai_app. */
	struct fyai_transport_state *transport;

	/* The event loop component, owned by struct fyai_app (src/loop/fyai_loop.h). */
	struct fyai_loop *loop;
	/* The output, the UI model and the screen, owned by struct fyai_app. */
	struct fyai_out *out;
	struct fyai_uistores *uistores;
	struct fyai_screen *screen;
	/* The sub-agent identity component, owned by struct fyai_app. */
	struct fyai_agent_state *astate;
	fy_generic tools;
	/* The turn component, owned by struct fyai_app. */
	struct fyai_turnstate *turn;
	/* The branch cursor component, owned by struct fyai_app. */
	struct fyai_branch_state *bstate;
	/* The components below are owned by struct fyai_app. */
	struct fyai_scope *scope;
	struct fyai_tty *tty;
	struct fyai_skills *skills;
	struct fyai_toolspec *toolspec;
	/* The MCP client component, owned by struct fyai_app. */
	struct fyai_mcp *mcp;
	/* The shared refresh of the login and its waiters (fyai_cred.h). */
	struct fyai_cred *cred;
	/* The tool jobs component, owned by struct fyai_app. */
	struct fyai_jobs *jobs;
	/* The waits component, owned by struct fyai_app. */
	struct fyai_waitq *waitq;
	/* The questions component, owned by struct fyai_app. */
	struct fyai_askq *askq;
	/* Fleet member state watcher, owned by struct fyai_app. */
	struct fyai_fleet_watch *fleet;
	/* ACP worker notifications share the active turn's event loop. */
	struct jsonrpc_conn *acp_rpc;
	const char *acp_agent_message_id;
	/* The usage accounting component, owned by struct fyai_app. */
	struct fyai_usage *usage;
};

/* Give a chunk of answer text to the observer of the context. */
static inline void fyai_text_observe(struct fyai_ctx *ctx, const char *text,
				     size_t len)
{
	if (ctx->turn->text_observe && len)
		ctx->turn->text_observe(ctx, text, len, ctx->turn->text_observe_data);
}

static inline bool fyai_interrupt_pending(const struct fyai_ctx *ctx)
{
	return ctx && ctx->loop && ctx->loop->interrupt_pending;
}


static inline bool fyai_interrupt_check(struct fyai_ctx *ctx)
{
	bool pending;

	if (!ctx || !ctx->loop)
		return false;
	pending = ctx->loop->interrupt_pending;
	ctx->loop->interrupt_pending = false;
	return pending;
}

/*
 * Copy @src for a computation whose diagnostics are of no use to the user,
 * such as the render of a stored transcript or a check of a document: the
 * copy has no sink, so it reports nothing, as a copy that is dropped did when
 * the sink was a member of the configuration. A copy that is adopted back
 * keeps the pointer to the sink: copy it plainly.
 */
static inline void fyai_cfg_copy_muted(struct fyai_cfg *dst,
				       const struct fyai_cfg *src)
{
	*dst = *src;
	dst->diag = NULL;
}

int
fyai_setup(struct fyai_ctx *ctx, struct fyai_app *app);

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
 * Keep @s for the life of the process. The copy is interned in the builder
 * of the configuration, which no arena reopen touches and which deduplicates,
 * so the pointer needs no free and the same value costs nothing twice. Use it
 * for a name, an id or a path that does not change for each turn. Do not use
 * it for text that is new for each turn, exchange or event, or for a secret.
 * Returns NULL for NULL, and for a failure after it reports it.
 */
const char *fyai_ctx_keep_str(struct fyai_ctx *ctx, const char *s);

/*
 * Run one complete tool-use loop on @turn; returns the final turn (or
 * fy_invalid on failure). Exposed for /compact's one-off summary request.
 * On an interrupted/failed run a diagnostic is attached to the returned
 * generic (FYGIF_DIAG indirect); when steps completed before the failure the
 * wrapped value is the partial turn, otherwise fy_invalid.
 */
fy_generic fyai_run_turn(struct fyai_ctx *ctx, fy_generic turn);

/*
 * A policy that a protocol front end puts on the tool calls of a turn. The
 * turn asks @before about each call of a model response before any of them
 * runs, and tells @after about each result before the next model request.
 * The turn waits on the event loop for each answer: fyai_tool_gate_allow()
 * or fyai_tool_gate_deny() for @before, fyai_tool_gate_done() for @after,
 * each with the round @id. An answer can come during the call. An answer to
 * a round that has ended is ignored. Streamed tool calls do not start before
 * the response ends while a gate is set.
 */
struct fyai_tool_gate_ops {
	void (*before)(struct fyai_ctx *ctx, uint64_t id, size_t index,
		       fy_generic call, void *data);
	void (*after)(struct fyai_ctx *ctx, uint64_t id, size_t index,
		      fy_generic call, fy_generic result, bool ok, void *data);
};

/* Let call @index of round @id run, with @args when they are valid. */
void fyai_tool_gate_allow(struct fyai_ctx *ctx, uint64_t id, size_t index,
			  fy_generic args);
/*
 * Answer call @index of round @id with @message in place of running it. @stop
 * ends the turn after the round.
 */
void fyai_tool_gate_deny(struct fyai_ctx *ctx, uint64_t id, size_t index,
			 const char *message, bool stop);
/*
 * End the report of result @index of round @id. @context, when given, goes to
 * the model before its next request. @stop ends the turn with @context as
 * its cause.
 */
void fyai_tool_gate_done(struct fyai_ctx *ctx, uint64_t id, size_t index,
			 const char *context, bool stop);

/*
 * Put the call of a delegated sub-agent before the gate of this context, in
 * a round of its own: @params is {phase: before, call} or {phase: after,
 * call, result, ok}. The answer goes back as the response to request @rpc_id
 * of the job that owns @branch: {allow, args, message, stop} or {context,
 * stop}. Returns -1 when no gate is set and the call needs no answer.
 */
int fyai_tool_gate_relay(struct fyai_ctx *ctx, const char *branch,
			 long long rpc_id, fy_generic params);
/* Forget the relayed rounds without an answer. */
void fyai_tool_gate_relays_drop(struct fyai_ctx *ctx);

/*
 * The conversation was replaced (resume, checkout, reset, clear): what the
 * model started in the old one does not report into the new one. End the
 * background sub-agents, monitors and named waits, and drop the queued
 * events. Terminal sessions belong to the user's screen and stay.
 */
void fyai_background_reset(struct fyai_ctx *ctx);
/* Give the queue of events the hooks of this run (wake the waiters, ask liveness). */
void fyai_waitq_bind(struct fyai_ctx *ctx);
/* Give the background questions their way to the user (this run). */
void fyai_askq_bind(struct fyai_ctx *ctx);
/* The state set aside for a line that runs beside a turn. */
struct fyai_scratch_state {
	struct fyai_transient saved;
	fy_generic held;
	bool entered;
};
int fyai_scratch_enter(struct fyai_ctx *ctx, struct fyai_scratch_state *st);
void fyai_scratch_leave(struct fyai_ctx *ctx, struct fyai_scratch_state *st);
fy_generic fyai_turn_append_event(struct fyai_ctx *ctx, fy_generic turn,
				  const struct fyai_event_input *input);

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
	       cfg->no_auth || ctx->transport->client;
}

#endif
