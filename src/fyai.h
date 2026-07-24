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
struct fyai_display_output;

#define OPENAI_RESPONSES_URL "https://api.openai.com/v1/responses"
#define OPENAI_CHAT_COMPLETIONS_URL "https://api.openai.com/v1/chat/completions"
#define ANTHROPIC_MESSAGES_URL "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION "2023-06-01"
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
/* Default rendered rows of a tool result shown in the display view. */
#define DEFAULT_TOOL_PREVIEW_LINES 5
#define DEFAULT_TOOL_UPDATE_INTERVAL_MS 33
/* Left indent applied to each rendered tool-output row (nests it under the
 * tool-call header), so the live loop and the history view match. */
#define FYAI_TOOL_OUTPUT_INDENT "    "
/* Default separators (markdown, themed by the renderer). The turn break is a
 * thematic-break rule; tool/section separators are empty (blank line only). */
#define DEFAULT_TURN_SEPARATOR "---"
#define DEFAULT_TOOL_SEPARATOR ""
#define DEFAULT_SECTION_SEPARATOR ""
/* Interactive prompt bubble: an empty prompt marker/top row keep the built-in
 * defaults; the bottom row is a {key} template reproducing the classic banner. */
#define DEFAULT_PROMPT_BOTTOM \
	" {model} · {provider} · {api}{effort}{summary}{temp}{ctx}"
/* Streaming markdown render cadence / colour / theme defaults. */
#define DEFAULT_MARKDOWN_MODE "line"	/* oneshot | line | stream */
#define DEFAULT_COLOR "auto"		/* auto | off | on */
#define DEFAULT_THEME "default:auto"	/* markdown theme[:auto|dark|light] */
#define DEFAULT_TOOL_DETAIL "default"	/* none | brief | default | full */
/* Display reasoning/thinking model output (live stream + history view). */
#define DEFAULT_THINKING true
/*
 * Durable arena root schema version. The root ref is a container mapping
 * { fyai: <version>, config: <doc|null>, catalog: <doc|null>, head: <turn|null> }
 * (a future "branches" key is reserved). Roots without the version key -
 * including pre-container turn-shaped roots - are rejected; re-init.
 */
#define FYAI_ROOT_VERSION 1

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
	int tool_update_interval_ms;
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
	bool async_model_step;
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
	/*
	 * Stack an in-memory builder over the durable arena so every config and
	 * state write this session is ephemeral (never published to the arena).
	 */
	bool transient;
	/* MCP (Model Context Protocol) server settings. */
	bool mcp_enabled;
	/* Hold the first model step until every server has settled (READY or
	 * FAILED), so the initial turn sees the complete toolset. False submits
	 * optimistically with whatever tools are ready. */
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
	/* Set when fyai_ctx_transient_gb() lazily created transient_gb for a
	 * caller with no active turn (e.g. an MCP startup step firing while the
	 * session is idle); the event-loop driver reclaims it. */
	bool transient_autorelease;
	CURL *curl;
	/* Per-invocation curl multi state. */
	struct fyai_curl_state *curl_state;

	/* The one application event loop. */
	struct fyai_event_loop *el;
	struct fyai_event_loop *event_loop_pool;
	struct fyai_event_source *event_source_pool;
	struct fyai_event_source *signal_src[5];
	sigset_t signal_mask;
	bool signal_mask_valid;
	struct fyai_ui *ui;
	bool interrupt_pending;
	bool terminate_pending;
	fy_generic tools;
	fy_generic last_message;
	fy_generic arena_config;	/* root["config"] or fy_invalid */
	fy_generic arena_catalog;	/* root["catalog"] or fy_invalid */
	uint64_t refs_head;
	struct curl_slist *headers;
	char *auth_header;
	char *user_agent;
	fy_generic mcp_tools;
	struct fyai_mcp_ctx *mcp;
	struct fyai_credentials auth;
	struct fy_generic_builder *auth_gb;
	bool auth_retry_done;
	bool stdout_tty;			/* stdout is a terminal (cached) */
	bool tool_output_displayed;
	/* The sole progressive transcript document for the active user or
	 * assistant output. Owned by this context, never by a signal handler. */
	struct fyai_display_output *display_output;
	struct fyai_fenced_stream *shell_stream; /* live progressive shell output */
	int tool_progress_fd;	/* child-to-parent progressive tool channel */
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
 * Return a transient builder to use as scratch for the current operation. If a
 * turn (or other caller) already holds one it is returned unchanged; otherwise
 * one is created on the spot and flagged transient_autorelease so the event-loop
 * driver releases it on the next iteration. Use this - not ctx->transient_gb
 * directly - from code that may run outside a turn (jsonrpc/MCP startup steps).
 */
struct fy_generic_builder *fyai_ctx_transient_gb(struct fyai_ctx *ctx);

/*
 * Run one complete tool-use loop on @turn; returns the final turn (or
 * fy_invalid on failure). Exposed for /compact's one-off summary request.
 * On an interrupted/failed run a diagnostic is attached to the returned
 * generic (FYGIF_DIAG indirect); when steps completed before the failure the
 * wrapped value is the partial turn, otherwise fy_invalid.
 */
fy_generic fyai_run_model_loop(struct fyai_ctx *ctx, fy_generic turn);

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

#endif
