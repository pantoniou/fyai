/*
 * fyai_claude_cli.h - the Claude Code CLI as a model connection
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The claude-code grammar sends each model request to an installed Claude
 * Code CLI over the stream-json protocol of the Claude Agent SDK. One CLI
 * process serves one exchange: the first model step starts it, and each
 * later step of the exchange gives it the results of the tools that it
 * asked for. The CLI has no built-in tools. It calls the tools of fyai
 * through an SDK MCP server named "fyai", and the requests of that server
 * wait until fyai has run each call through its own approval and tool path.
 *
 * A CLI whose turn ended well is kept for the next exchange, which it
 * continues with the new user messages alone, while the branch grows only by
 * its own answers and those messages and the model, the system prompt and
 * the tools stay. Any other request starts a new CLI that reads the branch.
 *
 * The bytes of a request are Anthropic Messages stream events in SSE form,
 * so the stream core reads them as it reads a Messages reply.
 *
 * Two hosts run a session. Without credential isolation the agent runs it, in
 * the slot of its link. With isolation the credential transport runs it, in
 * the slot of the channel of each execution, and the agent sends its requests
 * there (fyai_transport_server.c): the CLI and its login stay out of every
 * agent and tool.
 */

#ifndef FYAI_CLAUDE_CLI_H
#define FYAI_CLAUDE_CLI_H

#include <stdbool.h>
#include <stddef.h>

struct fyai_ctx;
struct fyai_claude_cli;
struct fyai_claude_cli_call;

/*
 * What the host gives a session: the CLI, whether it may bill a key, and
 * whether the host runs in a view, where the home is read-only and the CLI
 * keeps all its state in its configuration directory.
 */
struct fyai_claude_cli_opts {
	const char *command;		/* NULL or "": claude */
	bool api_billing;
	bool in_view;
};

/*
 * The configuration directory of the CLI: $CLAUDE_CONFIG_DIR, else
 * $HOME/.claude. False when neither names an absolute path.
 */
bool fyai_claude_cli_config_dir(char *out, size_t size);

/* How a request ended. */
enum fyai_claude_cli_end {
	FYAI_CLAUDE_CLI_OK,		/* the reply ended */
	FYAI_CLAUDE_CLI_CANCELLED,	/* fyai_claude_cli_cancel() */
	FYAI_CLAUDE_CLI_LOST,		/* the CLI went away; it can be retried */
	FYAI_CLAUDE_CLI_FAILED,		/* a failure that a retry cannot fix */
};

struct fyai_claude_cli_callbacks {
	/* The bytes of the reply; return @len to take them. */
	size_t (*body)(const void *data, size_t len, void *userdata);
	/*
	 * The request ended. @status is the HTTP status that the CLI reports
	 * for an API error, else 200 or 0; @error is the text of the failure.
	 * It runs from the event loop, never from the submit call.
	 */
	void (*end)(enum fyai_claude_cli_end end, long status,
		    const char *error, void *userdata);
	void *userdata;
};

/*
 * Send the request @body of @len bytes, a claude-code grammar request in
 * JSON. The session of @slot continues when it waits for the results that
 * @body gives; otherwise a new session starts there with @opts. NULL, with a
 * diagnostic, when the request cannot start. @slot stays valid until the host
 * releases it.
 */
struct fyai_claude_cli_call *
fyai_claude_cli_submit(struct fyai_ctx *ctx, struct fyai_claude_cli **slot,
		       const struct fyai_claude_cli_opts *opts,
		       const char *body, size_t len,
		       const struct fyai_claude_cli_callbacks *cb);

/* Stop the request and its CLI; the end callback still runs. */
void fyai_claude_cli_cancel(struct fyai_claude_cli_call *call);

/* Free a request; a request that has not ended ends without its callback. */
void fyai_claude_cli_call_destroy(struct fyai_claude_cli_call *call);

/*
 * The exchange ended. A CLI whose turn did not end well, such as one that
 * waits for the results of a cancelled exchange, is stopped from the loop;
 * any other is kept for the next exchange.
 */
void fyai_claude_cli_exchange_end(struct fyai_claude_cli **slot);

/*
 * The host goes: empty @slot and stop its CLI from the loop. The CLI sees EOF
 * and leaves.
 */
void fyai_claude_cli_release(struct fyai_claude_cli **slot);

/* Empty @slot, and stop and reap its CLI now. */
void fyai_claude_cli_cleanup(struct fyai_claude_cli **slot);

/*
 * Forget the CLI of the parent in a forked child: close the descriptors of
 * this process and touch neither the loop nor the program.
 */
void fyai_claude_cli_abandon(struct fyai_claude_cli **slot);

#endif
