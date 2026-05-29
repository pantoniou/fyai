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
#include <stdint.h>
#include <limits.h>

#include <curl/curl.h>
#include <libfyaml.h>
#include <libfyaml/libfyaml-allocator.h>
#include <libfyaml/libfyaml-generic.h>

#include "utils.h"
#include "commands.h"

#define OPENAI_RESPONSES_URL "https://api.openai.com/v1/responses"
#define OPENAI_CHAT_COMPLETIONS_URL "https://api.openai.com/v1/chat/completions"
#define ANTHROPIC_MESSAGES_URL "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION "2023-06-01"
#define DEFAULT_OPENAI_MODEL "gpt-5.4-mini"
#define DEFAULT_ANTHROPIC_MODEL "claude-sonnet-5"
/* Anthropic requires an explicit output-token cap on every request. */
#define DEFAULT_MAX_TOKENS 8192
#define DEFAULT_SYSTEM_PROMPT "You are a concise assistant."
#define MAX_TOOL_LOOP_ITERATIONS 16
#define DEFAULT_TEMPERATURE 0.0
/* Default lines of a tool result shown in the display view before truncation. */
#define DEFAULT_TOOL_PREVIEW_LINES 3
/* Streaming markdown render cadence / colour / theme defaults. */
#define DEFAULT_MARKDOWN_MODE "line"	/* oneshot | line | stream */
#define DEFAULT_COLOR "auto"		/* auto | off | on */
#define DEFAULT_THEME "auto"		/* auto | dark | light */
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
	const char *model;
	const char *api_key;
	/*
	 * Set when the key was supplied explicitly (--api-key or a config
	 * api_key env mapping); a mid-session /model switch keeps it. A key
	 * derived from the provider's <PROVIDER>_API_KEY env var is not
	 * explicit and is re-derived for the new provider.
	 */
	bool api_key_explicit;
	const char *provider;
	const char *prompt;
	const char *reasoning_effort;
	const char *reasoning_summary;
	const char *markdown_mode;	/* oneshot | line | stream */
	const char *color;		/* auto | off | on */
	const char *theme;		/* auto | dark | light (resolved in setup) */
	const char *code_theme;		/* libfyts styling name/path for fenced code */
	const char *markdown_theme;	/* named palette -> stylings/fyai-<name>.yaml */
	const char *markdown_style;	/* path override for the styling YAML */
	fy_generic markdown_style_doc;
	const char *markdown_code_theme;
	const char *markdown_rev_on[2];
	const char *markdown_rev_off[2];
	char markdown_style_path[PATH_MAX];
	int max_tool_iterations;
	int max_tokens;			/* output cap (required by Messages) */
	int top_logprobs;
	int tool_preview_lines;
	float temperature;
	bool enable_tools;
	bool enable_builtin_shell;
	bool enable_sandbox;	/* Landlock-confine shell tool sub-executions */
	fy_generic sandbox;	/* config sandbox: mapping (allow/deny/network) */
	bool interactive;
	int debug;
	bool pretty;
	bool markdown;
	bool cache_info;
	bool stats;
	bool stream;
	bool wire_logging;
	bool stream_logging;
	bool conversation_logging;
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
	bool response_chain;
	bool new_conversation;
	/*
	 * Stack an in-memory builder over the durable arena so every config and
	 * state write this session is ephemeral (never published to the arena).
	 */
	bool transient;
	const char *arena_dir;
	/* Repo arena catalog document (internalized into gb at config load;
	 * fy_invalid when the arena carries none - the embedded snapshot is
	 * the fallback). */
	fy_generic catalog;
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
	} config_ops[32];
	size_t config_op_count;

	/* the info about the command */
	struct fyai_cmd_info cmd;
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
	CURL *curl;
	fy_generic tools;
	fy_generic last_message;
	fy_generic arena_config;	/* root["config"] or fy_invalid */
	fy_generic arena_catalog;	/* root["catalog"] or fy_invalid */
	uint64_t refs_head;
	struct curl_slist *headers;
	char *auth_header;
	char *user_agent;
	bool stdout_tty;			/* stdout is a terminal (cached) */
	bool tool_output_displayed;
	bool shell_live_open;
	struct response_buffer shell_live_line;
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
};

/*
 * Settings carried over from the most recent stored turn so a continuation
 * defaults to what the conversation was using - model (pinned to the recorded
 * provider), API grammar, sampling parameters; /model and /api switches stick.
 * The command line still overrides and --new escapes. Strings are interned
 * into cfg->gb and freed with the config builder. Release with
 * fyai_last_turn_cleanup() (now a memset-only no-op for the strings).
 */
struct fyai_last_turn {
	const char *provider;
	const char *model;
	const char *api;
	const char *reasoning_effort;
	const char *reasoning_summary;
	double temperature;
	bool has_temperature;
};

void
fyai_peek_last_turn(struct fyai_cfg *cfg, struct fyai_last_turn *out);

void
fyai_last_turn_cleanup(struct fyai_last_turn *lt);

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
int fyai_reset_transient_builder(struct fyai_ctx *ctx);

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
fy_generic fyai_report_diag(fy_generic v);

/*
 * (Re)build the per-session request state derived from cfg: auth header,
 * header list, endpoint URL and the tools document. Requires an active
 * transient builder. Used at setup and after a mid-session /model switch.
 */
int fyai_request_state_apply(struct fyai_ctx *ctx);

#endif
