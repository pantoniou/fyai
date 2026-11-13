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

#define FYAI_SHELL_NO_COMMAND \
	"tool error: the shell tool needs a string argument \"command\""

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

/*
 * The phase of the tool/run request of a job. It only moves forward, so a
 * test may order it. A job is complete when its request is settled and, for a
 * job that has no terminal session, when its child is reaped too: the reap
 * is a process fact that is independent of the request and stays in `reaped`.
 */
enum fyai_tool_job_phase {
	FYAI_JOB_SPAWNED,	/* forked; the channel is not attached */
	FYAI_JOB_RUNNING,	/* the tool/run request is outstanding */
	FYAI_JOB_ANSWERED,	/* the request is settled; the child may live */
	FYAI_JOB_DONE,		/* complete: the result can be collected */
};

/*
 * What the job reported. A failed control channel is final: a later verdict
 * from an answer or from an exit status does not replace it.
 */
enum fyai_tool_job_verdict {
	FYAI_JOB_UNDECIDED,
	FYAI_JOB_OK,
	FYAI_JOB_NOT_OK,
	FYAI_JOB_CHANNEL_FAILED,
};

/*
 * The job owns its source pointers so withdrawing is idempotent: the callbacks
 * retire a source when it is spent and the collect path retires whatever is
 * left, and neither has to know what the other did. The loop is shared and
 * outlives the job, so a source left behind would point at freed storage.
 */
struct fyai_tool_job {
	struct fyai_ctx *ctx;		/* the loop the job's sources live on */
	fy_generic call;		/* the tool call, sent as tool/run */
	struct fy_generic_builder *call_gb; /* owns the call across turn cleanup */
	struct jsonrpc_conn *conn;	/* control channel to the child */
	struct jsonrpc_request *run;	/* the outstanding tool/run */
	fy_generic result;
	fy_generic display;		/* tool-resolved presentation, if any */
	fy_generic diag;		/* diagnostics collected by the child */
	char *origin;			/* who the child was, for a diagnostic */
	long long model_text;		/* the bytes of its terminal, for the model */
	pid_t pid;
	uint64_t transport_exec;	/* the transport's id for this sub-agent, or 0 */
	int rfd;
	int pfd;
	struct fyai_fenced_stream stream;
	struct fyai_sink_band *band;
	struct fyai_event_source *band_delay;
	struct fyai_buf pending_output;
	char *title;
	char *command;
	struct fyai_event_source *csrc;
	enum fyai_tool_job_phase phase;
	enum fyai_tool_job_verdict verdict;
	bool have_result;	/* the child answered tool/run */
	bool reaped;		/* the child has been waited for */
	bool native_shell;
	bool agent;			/* the call delegates to a sub-agent */
	bool btw;			/* the user opened this side question */
	bool btw_panel;		/* its finished screen remains in the pane */
	bool exec;			/* the child executes fyai again */
	bool band_progress;
	int pty;			/* terminal of a sub-agent, -1 if none */
	int pty_rows, pty_cols;
	/* The size of the tile of a terminal session, which its program
	 * starts at, or 0. */
	int start_rows, start_cols;
	/* What the tile draws: the work pane decides it from the grant. */
	enum fyai_workpane_present present;
	struct fyai_terminal_view *view;	/* what it drew there */
	struct fytim_surface *surface;		/* and where that is shown */
	/* The screen is a block of the transcript at the call, not a tile. */
	bool inline_block;
	/* The rows that separated the block when it was painted. */
	int inline_sep;
	struct fyai_event_source *ptysrc;
	struct fyai_event_source *animation;
	size_t animation_frame;
	struct fyai_event_source *waiter;	/* asks if it stopped for input */
	bool wants_input;
	/*
	 * One-way latches that are set before the outcome is known and do not
	 * order with the phase: a stop was requested, the time limit expired,
	 * the advisory limit of an agent passed.
	 */
	bool terminating;
	bool timed_out;
	bool overdue;
	unsigned int timeout_ms;	/* 0 = no limit */
	unsigned int hang_timeout_ms;
	fyai_event_ms_t started_ms;
	fyai_event_ms_t elapsed_ms;
	struct fyai_buf progress;	/* tail, for a timeout report */
	char *branch;			/* sub-agent branch, allocated by us */
	char *monitor;			/* monitor that this command feeds */
	struct fyai_event_source *deadline;
	struct fyai_event_source *hang_deadline;
	int term_signal;
	struct fyai_tool_job_group *group;
	struct fyai_tool_job *next;	/* ctx->tool_jobs, for a resize */
	struct fyai_shell_session *session;	/* the session this job drives */
	struct fyai_view_run *view_run;	/* the view of an isolated sub-agent */
	bool view_entry_failed;		/* the child could not enter that view */
};

/*
 * Why a session was asked to end. A request is never withdrawn, and it stays
 * after the program ends, because the cause of the end reads it. A stop for
 * idleness is a close too, so the reasons order: a later one replaces an
 * earlier one only by raising it.
 */
enum fyai_session_stop {
	FYAI_SESSION_STOP_NONE,
	FYAI_SESSION_STOP_CLOSE,	/* closed by the model or the user */
	FYAI_SESSION_STOP_IDLE,		/* the idle limit expired */
};

/* A named job whose terminal view remains readable after exit. */
struct fyai_shell_session {
	struct fyai_shell_session *next;
	struct fyai_ctx *ctx;
	struct fyai_scope_pin pin;	/* it holds the scope of its exchange */
	/* The bytes of the terminal, a text of the UI model for its consumer. */
	long long model_text;
	char *name;
	char *command;
	char *branch;
	struct fyai_tool_job *job;	/* NULL once the process has gone */
	struct fyai_terminal_view *view;
	struct fyai_event_source *idle;
	/* The session owns one live surface until its program stops. */
	struct fyai_event_source *animation;
	size_t animation_frame;
	struct fytim_surface *surface;
	char *title;
	char *description;		/* model-provided call description */
	bool recorded;			/* exchange stored in the document */
	/* A bang shell: its tile stays after the program until the user
	 * dismisses it. */
	bool keep_tile;
	/* The user closed the tile: it goes when the program ends. */
	bool close_on_exit;
	pid_t pid;			/* the program, watched for a read */
	bool pipes;			/* it was given no terminal */
	struct fyai_event_source *waiter;
	bool wants_input;		/* it stopped for input and was said so */
	fyai_event_ms_t quiet_since;	/* when its terminal last had output */
	bool feeding;			/* keystrokes are on their way to it */
	bool drew;			/* its terminal had output one time */
	int rows;			/* the size the session was opened with */
	int exit_code;
	int signal;
	/*
	 * The end of the program is a one-way latch of its own: a close waits
	 * on it, and nothing but the exit handshake sets it. `wants_input`
	 * toggles while the program runs, `keep_tile` and `close_on_exit`
	 * are independent requests, and none of them orders with the stop.
	 */
	bool exited;
	enum fyai_session_stop stop;
	bool user_owned;
	/* Told when the program ends, for a program the user runs through fyai. */
	fyai_tools_exit_fn on_exit;
	void *on_exit_data;
	/* What the tile draws: the work pane decides it from the grant. */
	enum fyai_workpane_present present;		/* opened by a bang command */
	int resize_rows;		/* pending child resize */
	int resize_cols;
	/* The screen is a block of the transcript at the call, not a tile. */
	bool inline_block;
	/* The rows that separated the block when it was painted. */
	int inline_sep;
	/* A program of the user, such as an editor, that takes the pane and
	 * the keys. */
	bool takes_pane;
	/* A mirror of a session of a remote worker: its program runs there,
	 * and its input, its size and its end go there through these. */
	const struct fyai_tools_remote *remote;
	void *remote_data;
};

#define FYAI_TOOL_TERM_MS 2000

enum fyai_tool_group_state {
	FYAITGS_QUEUED,
	FYAITGS_RUNNING,
	FYAITGS_PARKED,
	FYAITGS_COLLECTED,
	FYAITGS_SUBMIT_FAILED,
};

struct fyai_tool_group_entry {
	fy_generic call;		/* held in the builder of the group */
	fy_generic result;
	bool has_result;
	struct fyai_tool_job *job;
	struct fyai_mcp_call_request *mcp_request;
	struct fyai_waiter *waiter;	/* a wait for an event of this group */
	enum fyai_tool_group_state state;
	bool parallel;
	bool result_ok;
};

/*
 * Where a group is in its life. Calls are added while it builds; it runs
 * when it is submitted, and takes no more calls once it is sealed. A cancel
 * seals it. The phase only moves forward.
 */
enum fyai_tool_group_phase {
	FYAITGP_BUILDING,	/* not submitted: nothing runs yet */
	FYAITGP_OPEN,		/* submitted: runs, and takes calls */
	FYAITGP_SEALED,		/* takes no more calls */
	FYAITGP_CANCELLED,	/* sealed, and nothing new starts */
};

struct fyai_tool_job_group {
	struct fyai_ctx *ctx;
	/* Holds the calls and results: the transient builder can go first. */
	struct fy_generic_builder *gb;
	struct fyai_tool_group_entry *entries;
	struct fyai_event_source *animation_timer;
	size_t count;
	size_t capacity;
	size_t next;
	size_t active;
	size_t parked;
	size_t max_parallel;
	enum fyai_tool_group_phase phase;
	bool exclusive;
	bool notified;
	fyai_tool_group_complete_fn complete;
	void *userdata;
};

/* fyai_tools.c */
bool fyai_shell_args_lack_command(const char *name, fy_generic args);
fy_generic fyai_tool_call_effective(struct fyai_ctx *ctx,
				    fy_generic tool_call);
bool fyai_shell_named_call(struct fyai_ctx *ctx, fy_generic tool_call);
bool fyai_shell_session_call(struct fyai_ctx *ctx, fy_generic tool_call);
char *fyai_format_tool_header(struct fyai_ctx *ctx, const char *tool,
			      fy_generic args, int preview_lines);
int fyai_shell_view(struct fyai_ctx *ctx, const char *command,
		    fy_generic args, char **title,
		    struct fyai_buf *body);
void fyai_print_tool_view(struct fyai_ctx *ctx, const char *title,
			  struct fyai_buf *body);
char *fyai_format_shell_label(fy_generic args);
void fyai_tool_progress_flush(struct fyai_ctx *ctx);
unsigned int fyai_shell_timeout_ms(struct fyai_ctx *ctx, fy_generic call,
				   bool native);
size_t fyai_shell_output_bytes(struct fyai_ctx *ctx, fy_generic call);
void fyai_shell_tty_size(struct fyai_ctx *ctx, fy_generic call,
			 int *rowsp, int *colsp);
bool fyai_shell_tty_requested(struct fyai_ctx *ctx, fy_generic call);
const char *fyai_shell_shell_requested(struct fyai_ctx *ctx,
				       fy_generic call);
bool fyai_shell_login_requested(struct fyai_ctx *ctx, fy_generic call);
char *fyai_shell_bound_alloc(const char *text, size_t max_bytes);
fy_generic fyai_ask_declined(struct fyai_ctx *ctx);
fy_generic fyai_ask_user_from(struct fyai_ctx *ctx, fy_generic args,
			      const char *from);
int fyai_tool_apply_sandbox(struct fyai_ctx *ctx);
fy_generic fyai_tool_result_note(struct fyai_ctx *ctx, bool agent_call,
				 fy_generic result);
bool fyai_tool_call_instant(struct fyai_ctx *ctx, fy_generic tool_call);

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

/* fyai_shell_session.c */
void fyai_tools_work_update(struct fyai_ctx *ctx);
void fyai_terminal_tap(struct fyai_ctx *ctx,
		       struct fyai_terminal_view *view,
		       long long *textp, const char *name,
		       const char *data, size_t len);
int fyai_inline_sep(struct fyai_ctx *ctx);
void fyai_inline_animation_arm(struct fyai_ctx *ctx,
		enum fyai_event_action (*fn)(const struct fyai_event *ev),
		void *owner, struct fyai_event_source **srcp);
bool fyai_shell_session_name_valid(const char *name);
extern const struct fyai_workpane_ladder fyai_tile_ladder;
bool fyai_tile_draws_program(enum fyai_workpane_present p);
struct fyai_shell_session *
fyai_shell_session_create(struct fyai_ctx *ctx, const char *name,
			  const char *command, const char *title,
			  const char *description, int rows,
			  int cols, size_t max_bytes, bool pipes,
			  bool user_owned, const struct fyai_tools_remote *remote,
			  void *remote_data);
void fyai_shell_session_start_size(struct fyai_tool_job *job,
				   fy_generic args);
void fyai_shell_session_resized(struct fyai_shell_session *sess,
				int rows, int cols);
void fyai_shell_session_refresh(struct fyai_shell_session *sess);
void fyai_shell_session_watch(struct fyai_shell_session *sess, pid_t pid);
int fyai_shell_inline_cols(struct fyai_ctx *ctx);
size_t fyai_inline_screen_trim(struct fyai_ctx *ctx, const char *screen,
			       size_t len);
void fyai_shell_session_inline_paint(struct fyai_shell_session *sess,
				     bool done);
void fyai_shell_session_inline_follow(struct fyai_shell_session *sess);
void fyai_surface_retire_zoom(struct fyai_ctx *ctx,
			      struct fytim_surface *surface);
void fyai_shell_session_exited(struct fyai_shell_session *sess,
			       int exit_code, int signal);
void fyai_shell_sessions_abandon(struct fyai_ctx *ctx);
char *fyai_shell_output_tool(struct fyai_ctx *ctx, fy_generic args,
			     bool *okp);
char *fyai_shell_input_tool(struct fyai_ctx *ctx, fy_generic args,
			    bool *okp);
char *fyai_shell_close_tool(struct fyai_ctx *ctx, fy_generic args,
			    bool *okp);

/* fyai_agent_view.c */
void fyai_agent_tty_size(struct fyai_ctx *ctx, int *rowsp, int *colsp);
void fyai_agent_inline_paint(struct fyai_tool_job *job, bool done,
			     bool ok);
void fyai_agent_view_refresh(struct fyai_tool_job *job);
void fyai_agent_head_paint(struct fyai_tool_job *job);
void fyai_agent_view_reply(const char *data, size_t len, void *user);
fy_generic fyai_agent_input_tool(struct fyai_ctx *ctx, fy_generic args,
				 bool *okp);
int fyai_agent_view_open(struct fyai_ctx *ctx,
			 struct fyai_tool_job *job);
void fyai_agent_view_close(struct fyai_tool_job *job, bool ok,
			   const char *cause);
bool fyai_agent_isolated(struct fyai_ctx *ctx, fy_generic args);
int fyai_agent_view_begin(struct fyai_ctx *ctx, const char *agent,
			  struct fyai_view_run **run);
void fyai_agent_view_finish(struct fyai_ctx *ctx, struct fyai_tool_job *job,
			    fy_generic *result);

/* fyai_tool_job.c */
void fyai_tool_jobs_abandon(struct fyai_ctx *ctx);
const char *fyai_agent_job_name(const struct fyai_tool_job *job);
struct fyai_tool_job *fyai_agent_job_named(struct fyai_ctx *ctx,
					   const char *name);
void fyai_tool_job_discard(struct fyai_tool_job *job);

#endif /* FYAI_TOOLS_INT_H */
