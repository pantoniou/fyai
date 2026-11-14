/*
 * fyai_mcp_state.h - the MCP client component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_MCP_STATE_H
#define FYAI_MCP_STATE_H

#include <stdbool.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_cred;
struct fyai_loop;
struct fyai_mcp_ctx;
struct fyai_transient;

/*
 * The connections to the MCP servers and what they offer: the head of the
 * list of connections, the tool catalogue folded from them and the names
 * activated from a large catalogue. The component stands on the loop for its
 * sources, on the credential component for the login of a server and on the
 * transient builder for the values of one call. The owner is struct
 * fyai_app.
 */
struct fyai_mcp {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_cred *cred;
	struct fyai_transient *transient;

	struct fyai_mcp_ctx *conns;	/* the connections, newest first */
	fy_generic tools;
	/* Names activated from a large MCP catalogue for this invocation. */
	fy_generic active_tools;
	bool stopping;
	/* The invocation added servers: MCP is on whatever the configuration says. */
	bool invocation;
};

void fyai_mcp_state_init(struct fyai_mcp *mcp, struct fyai_loop *loop,
			 struct fyai_cred *cred, struct fyai_transient *transient);

#endif
