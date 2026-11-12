/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TOOLS_INT_H
#define FYAI_TOOLS_INT_H

/*
 * Internal interface of the tool code: fyai_tools.c, fyai_shell_sandbox.c,
 * fyai_tool_child.c, fyai_shell_session.c, fyai_agent_view.c and
 * fyai_tool_job.c. Nothing here is public.
 */

#include "fyai_tools.h"
#include "fyai_sandbox.h"
#include "fyai_event.h"
#include "fyai_markdown.h"
#include "fyai_workpane.h"

/*
 * Per-call sandbox spec plus the owned backing storage its pointers reference.
 * The spec must outlive the fork inside run_shell_command_capture_cb, so this
 * lives on the caller's stack and fyai_shell_sandbox_end() frees the arrays.
 */
struct fyai_shell_sandbox {
	struct fyai_sandbox_spec spec;
	char *root;
	const char **hide;			/* each entry owned; covered in the tool */
	size_t hide_n;
	struct fyai_sandbox_path *allow;	/* each .path owned; see fyai_shell_sandbox_allow() */
	size_t allow_cap;
	const char **deny;			/* each entry owned */
	uint16_t *tcp_ports;
	uint16_t *tcp_bind_ports;
	uint16_t *udp_ports;
	uint16_t *udp_bind_ports;
};

/* fyai_shell_sandbox.c */
int fyai_shell_sandbox_build(struct fyai_ctx *ctx,
			     struct fyai_shell_sandbox *sb,
			     const struct fyai_sandbox_spec **specp);
int fyai_shell_sandbox_begin(struct fyai_ctx *ctx,
			     struct fyai_shell_sandbox *sb,
			     const struct fyai_sandbox_spec **specp);
void fyai_shell_sandbox_end(struct fyai_shell_sandbox *sb);

#endif /* FYAI_TOOLS_INT_H */
