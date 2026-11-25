/*
 * fyai_turnstate.h - the turn component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_TURNSTATE_H
#define FYAI_TURNSTATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct fyai_diag;
struct fyai_gate_relay;
struct fyai_tool_gate_ops;
struct fyai_turn_run;

enum fyai_turn_stop {
	FYAI_TURN_STOP_NONE,
	FYAI_TURN_STOP_LIMIT,		/* max_tool_iterations */
	FYAI_TURN_STOP_COST,		/* the cost limit of the turn */
	FYAI_TURN_STOP_REQUEST,		/* the provider request failed */
};

enum fyai_chain_state {
	FYAI_CHAIN_UNLINKED,
	FYAI_CHAIN_LINKED,
	FYAI_CHAIN_MISSED,
};

/*
 * What a running turn carries: the last message, the observers of a front
 * end, the tool gate and its rounds, why the turn stopped, the schema of the
 * answer, the cost limit, the chain state of the request, the token extents
 * and the attachments that wait. The component reports into the diagnostics.
 * The owner is struct fyai_app; a view context keeps a copy of its own.
 */
struct fyai_turnstate {
	/* Dependency. */
	struct fyai_diag *diag;

	fy_generic last_message;
	/* Do not apply the context guard to a compaction request. */
	bool compacting;
	/* Auto-titling already attempted in this process; do not re-trigger. */
	bool title_tried;
	/* Output allowance after the context check. Zero uses the configuration. */
	long long context_max_tokens;
	/*
	 * Called with each turn that a running turn appends: a model response
	 * and each tool result. The turn is valid during the call. A protocol
	 * front end streams the conversation from it.
	 */
	void (*observe)(struct fyai_ctx *ctx, fy_generic turn, void *data);
	void *observe_data;
	/*
	 * Called with each step that a delegated sub-agent appends to its
	 * turn: {messages, usage, model}. @call is the agent call of this
	 * turn that started it. The step is valid during the call.
	 */
	void (*agent_observe)(struct fyai_ctx *ctx, fy_generic call,
			      fy_generic step, void *data);
	void *agent_observe_data;
	/* Called with each chunk of answer text as it arrives. */
	void (*text_observe)(struct fyai_ctx *ctx, const char *text, size_t len,
			     void *data);
	void *text_observe_data;
	/*
	 * The lines the user typed during a turn, when this program has no
	 * display to hold them: a worker takes them from its client. Peek
	 * returns the first line or NULL; take removes it and returns a copy.
	 */
	const char *(*line_peek)(struct fyai_ctx *ctx, void *data);
	char *(*line_take)(struct fyai_ctx *ctx, void *data);
	void *line_data;
	/*
	 * Why the last turn stopped short. A turn sets it; a caller that reports
	 * it clears it before the turn.
	 */
	enum fyai_turn_stop stop;
	/*
	 * The JSON Schema of the answer that a front end asks for, or fy_invalid,
	 * and the answer that the structured_output tool accepted.
	 */
	fy_generic output_schema;
	fy_generic structured_output;
	/* Stop before a model request once the cost reaches this; 0 for none. */
	double cost_limit;
	/*
	 * A tool asks the turn to end before its next model request. A request
	 * that the turn consumes, not a phase: @stop records why a turn
	 * stopped, and the two do not describe the same event.
	 */
	bool end_requested;
	/*
	 * Optional handoff hook for /background (NULL if unsupported). Moves
	 * the session and any in-flight turn to the project server. Returns 0
	 * on success, -1 with diagnostic on error.
	 */
	int (*background)(struct fyai_ctx *ctx, void *data);
	void *background_data;
	/*
	 * Optional follow-up action passed to @background by its caller:
	 * NULL (exit), "" (new session), or target branch/command string.
	 */
	const char *background_next;
	/*
	 * Optional front-end busy probe (NULL to rely on local UI state).
	 */
	bool (*working)(struct fyai_ctx *ctx, void *data);
	/* Set by /continue to trigger a model turn on the existing head. */
	bool continue_requested;
	/* The tool gate of the turns of this context, and its open round. */
	const struct fyai_tool_gate_ops *gate;
	void *gate_data;
	struct fyai_turn_run *gate_run;
	uint64_t gate_seq;
	/* The rounds that delegated sub-agents asked the gate for. */
	struct fyai_gate_relay *gate_relays;
	/*
	 * A delegated sub-agent: the parent asks for its tool calls to go
	 * through the gate of the parent, and for each step of its turns.
	 */
	bool relay_gate;
	bool relay_steps;
	/*
	 * While set, each tool call that the turn answers adds {call, result,
	 * ok} to tool_results (fyai_run_tool_calls).
	 */
	bool collect_tool_results;
	fy_generic tool_results;
	/* Index of the next pre-supplied --answer to hand to ask_user. */
	size_t answer_next;
	/* Set when ask_user needs an answer but none can be obtained
	 * (non-interactive stdin with no --answer left); aborts the run. */
	bool ask_abort;
	/* Token extents collected by the last streamed call (fy_invalid when
	 * none); consumed when the assistant response is appended to a turn. */
	fy_generic last_token_extents;
	/* One-way latch, not a phase: set when a provider rejected the logprobs
	 * params injected for token_extents, so the session stops asking. */
	bool token_extents_off;
	/* The chain state of the last request. A request that carries no
	 * previous_response_id leaves it UNLINKED. */
	enum fyai_chain_state response_chain;
	/*
	 * The attachments that the next user message takes, as canonical image
	 * parts that hold their bytes.
	 */
	fy_generic media_pending;
};

void fyai_turnstate_init(struct fyai_turnstate *turn, struct fyai_diag *diag);
/* Forget what a forked child must not keep of its parent's turn. */
void fyai_turnstate_abandon(struct fyai_turnstate *turn);

#endif
