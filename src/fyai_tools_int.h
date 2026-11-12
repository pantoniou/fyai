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

/* A sub-agent in a view announces its process to the supervisor on this one. */
#define FYAI_TOOL_CHILD_VIEW_FD 7

/* fyai_tools.c */
bool fyai_shell_session_call(struct fyai_ctx *ctx, fy_generic tool_call);
void fyai_shell_tty_size(struct fyai_ctx *ctx, fy_generic call,
			 int *rowsp, int *colsp);
bool fyai_shell_tty_requested(struct fyai_ctx *ctx, fy_generic call);
const char *fyai_shell_shell_requested(struct fyai_ctx *ctx,
				       fy_generic call);
bool fyai_shell_login_requested(struct fyai_ctx *ctx, fy_generic call);
int fyai_tool_apply_sandbox(struct fyai_ctx *ctx);
fy_generic fyai_tool_result_note(struct fyai_ctx *ctx, bool agent_call,
				 fy_generic result);

/* fyai_shell_sandbox.c */
int fyai_shell_sandbox_build(struct fyai_ctx *ctx,
			     struct fyai_shell_sandbox *sb,
			     const struct fyai_sandbox_spec **specp);
int fyai_shell_sandbox_begin(struct fyai_ctx *ctx,
			     struct fyai_shell_sandbox *sb,
			     const struct fyai_sandbox_spec **specp);
void fyai_shell_sandbox_end(struct fyai_shell_sandbox *sb);

/* fyai_tool_child.c */
int fyai_tool_child_tty(int slave);
int fyai_tool_child_fds(struct fyai_ctx *ctx, int req_fd, int rsp_fd, int tp_agent_fd,
			int tp_ctl_fd, int view_fd, bool keep_arena);
void fyai_tool_child_serve_loop(struct fyai_ctx *ctx);
void fyai_tool_child_signals_block(void);
void fyai_tool_child_signals(struct fyai_ctx *ctx);
bool fyai_agent_spawn_exec(struct fyai_ctx *ctx);
fy_generic fyai_agent_spawn_state(struct fyai_ctx *ctx,
				  struct fy_generic_builder *gb,
				  uint64_t transport_exec);
void fyai_tool_child_exec(struct fyai_ctx *ctx, bool pty, bool tp);

#endif /* FYAI_TOOLS_INT_H */
