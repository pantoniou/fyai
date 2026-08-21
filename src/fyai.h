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

#include "utils.h"
#include "commands.h"
#include "fyai_auth.h"
#include "fyai_diag.h"

struct fyai_fenced_stream;	/* live progressive shell output (fyai_markdown.h) */
struct fyai_ui;
struct jsonrpc_conn;
struct fyai_display_output;
struct fyai_tool_job;
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
#define MAX_TOOL_LOOP_ITERATIONS 50
#define DEFAULT_TEMPERATURE 0.0
/* Shell time limits in milliseconds. Zero disables either limit. */
#define DEFAULT_RETRY_MAX_ATTEMPTS 8
#define DEFAULT_RETRY_INITIAL_DELAY_MS 500
#define DEFAULT_RETRY_MAX_DELAY_MS 30000
#define DEFAULT_SHELL_TIMEOUT_MS 120000
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
#define DEFAULT_SHELL_HARD_MAX_OUTPUT_TOKENS 200000
/* The bytes/4 rule the context estimator uses, in one place. */
#define FYAI_BYTES_PER_TOKEN 4
/* Default rendered rows of a tool result shown in the display view. */
#define DEFAULT_TOOL_PREVIEW_LINES 5
/* Interactive history recap: -1 fills the screen, 0 is off, N is a count. */
#define DEFAULT_RECAP_EXCHANGES (-1)
#define DEFAULT_TOOL_UPDATE_INTERVAL_MS 33
/* Left indent applied to each rendered tool-output row (nests it under the
 * tool-call header), so the live loop and the history view match. */
#define FYAI_TOOL_OUTPUT_INDENT "    "
/* Chrome at the left of a terminal session, so its screen reads as one
 * thing. */
#define FYAI_SESSION_MARGIN "│ "
/* Mark shell commands and align continuation rows. */
#define FYAI_TOOL_MARKER "⎿  "
#define FYAI_TOOL_MARKER_PAD "   "
/* Display columns of both, for the layout that has to reserve them. */
#define FYAI_TOOL_MARKER_WIDTH 3
/* Default separators (markdown, themed by the renderer). The turn break is a
 * thematic-break rule; tool/section separators are empty (blank line only). */
#define DEFAULT_TURN_SEPARATOR "---"
#define DEFAULT_TOOL_SEPARATOR ""
#define DEFAULT_SECTION_SEPARATOR ""
/* Interactive prompt bubble: an empty prompt marker/top row keep the built-in
 * defaults; the bottom row is a {key} template reproducing the classic banner. */
#define DEFAULT_PROMPT_BOTTOM \
	" {model} · {provider} · {api}{effort}{summary}{temp}" \
	"{tokens}{cache}{cost}"
/* Streaming markdown render cadence / colour / theme defaults. */
#define DEFAULT_MARKDOWN_MODE "line"	/* oneshot | line | stream */
#define DEFAULT_MARKDOWN_UPDATE_INTERVAL_MS 50
#define DEFAULT_COLOR "auto"		/* auto | off | on */
#define DEFAULT_THEME "default:auto"	/* markdown theme[:auto|dark|light] */
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
#define FYAI_ROOT_VERSION 2

/* The branch an arena starts on, and the fallback when none is selected. */
#define FYAI_BRANCH_DEFAULT "main"

/* Maximum nesting depth of sub-agent branches below a top-level branch. */
#define DEFAULT_AGENT_MAX_BRANCH_DEPTH 8
#define DEFAULT_AGENT_MAX_TIMEOUT_MS 3600000

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

struct fyai_cfg {
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
	const char *theme_variant;	/* resolved dark | light */
	const char *markdown_theme;	/* resolved libfymd4c theme name */
	const char *markdown_rev_on[2];	/* reverse-card pair, [0] dark [1] light */
	const char *markdown_rev_off[2];
	const char *turn_separator;	/* history inter-turn break (markdown) */
	const char *tool_separator;	/* rendered before a tool result (markdown) */
	const char *section_separator;	/* reasoning -> answer break (live stream) */
	const char *prompt_marker;	/* interactive prompt marker (SGR ok) */
	const char *prompt_top;		/* REPL bubble top row template (SGR ok) */
	const char *prompt_bottom;	/* REPL bubble bottom {key} template (SGR ok) */
	int table_border;		/* 0 theme (default) | 1 grid | 2 none */
	int max_tool_iterations;
	int max_tokens;			/* output cap (required by Messages) */
	int top_logprobs;
	int tool_preview_lines;
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
	const char *session_margin;	/* left chrome of a terminal session */
	bool agent_pty;			/* this sub-agent has a terminal */
	int shell_tty_rows;		/* PTY rows (0 = follow the terminal) */
	int shell_tty_cols;		/* PTY columns (0 = follow the terminal) */
	int agent_timeout_ms;		/* sub-agent time limit (0 = none) */
	int agent_max_timeout_ms;	/* bound on a model-asked limit (0 = none) */
	int agent_max_branch_depth;	/* nesting cap for sub-agent branches */
	const char *tool_detail;
	bool transcript_system;
	float temperature;
	bool enable_tools;
	bool parallel_tool_calls;
	bool enable_builtin_shell;
	bool enable_sandbox;	/* Landlock-confine shell tool sub-executions */
	fy_generic sandbox;	/* config sandbox: mapping (allow/deny/network) */
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
	bool agent_child;
	/* The parent limits a forked tool job. */
	bool tool_child;
	/* Serve the agent protocol on standard input and output. */
	bool agent_rpc;
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
	/* Repo arena catalog document (internalized into gb at config load;
	 * fy_invalid when the arena carries none - the embedded snapshot is
	 * the fallback). */
	fy_generic catalog;
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
	struct fyai_diag diag;
};

static inline const struct fyai_verb *
fyai_cfg_verb(struct fyai_cfg *cfg)
{
	if (!cfg)
		return NULL;

	return fyai_id_to_verb(cfg->cmd.id);
}

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

static inline bool
fyai_cfg_uses_storage(struct fyai_cfg *cfg)
{
	return !fyai_cfg_no_storage(cfg);
}

struct fyai_mcp_ctx;
struct fyai_config_edit_request;

struct fyai_event_loop;
struct fyai_event_source;

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
	struct fyai_config_edit_request *config_edit;
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
	fy_generic arena_catalog;	/* root["catalog"] or fy_invalid */
	/* Active branch state and its next ref-log predecessor. */
	char *branch;
	/* Stored HEAD, which can differ from the active branch. */
	char *head_branch;
	/* Durable branch for a sub-agent conversation. */
	char *agent_branch;
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
	/* Named terminal sessions, each one a process of its own. The view of
	 * a session lives here and so outlives the process that drove it. */
	struct fyai_shell_session *shell_sessions;
	struct fyai_wait *waits;	/* named waits, live for this run */
	/* Events queued for model turns in arrival order. */
	struct fyai_pending_event *events;
	struct fyai_pending_event **events_tail;
	struct fyai_event_source *winch_src;
	bool tool_output_displayed;
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
	int usage_calls;
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
/* Take the oldest queued event, or NULL. The caller owns it. */
char *fyai_event_take(struct fyai_ctx *ctx);
bool fyai_event_queued(const struct fyai_ctx *ctx);
void fyai_events_release(struct fyai_ctx *ctx);

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

#endif
