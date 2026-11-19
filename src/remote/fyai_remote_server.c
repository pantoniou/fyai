/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_config.h"
#include "fyai_event.h"
#include "fyai_fleet.h"
#include "fyai_host.h"
#include "fyai_jsonrpc.h"
#include "fyai_remote.h"
#include "fyai_remote_http.h"
#include "fyai_remote_int.h"
#include "fyai_claude_sdk.h"
#include "fyai_remote_tls.h"
#include "fyai_sink.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
#include "fyai_transport_boot.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_REMOTE

/* The descriptor that the project server of a server of projects takes clients on. */
#define REMOTE_FRONT_FD		3

/* The environment variable that names the descriptor `serve --daemon` reports on. */
#define REMOTE_READY_ENV	"FYAI_SERVE_READY_FD"

/* The descriptors that a worker finds the credential transport on. */
#define REMOTE_WORKER_AGENT_FD	3
#define REMOTE_WORKER_CTL_FD	4
#define REMOTE_WORKER_SYNC_FD	5

/*
 * recvmsg() flag that makes a received descriptor close-on-exec. macOS has
 * none: remote_recv_fd() marks the descriptor after the receive.
 */
#ifdef MSG_CMSG_CLOEXEC
#define REMOTE_RECV_FLAGS	MSG_CMSG_CLOEXEC
#else
#define REMOTE_RECV_FLAGS	0
#endif

/* ---- the server ------------------------------------------------------- */

struct fyai_remote_server;
struct fyai_remote_session;

struct fyai_remote_run;

/* What a client is. It moves forward only, and a failed peer keeps its role. */
enum remote_peer_role {
	REMOTE_PEER_PENDING,	/* it has not shown the launch token */
	REMOTE_PEER_CLIENT,	/* it drives a command or a session */
	REMOTE_PEER_WATCHER,	/* it reads a session that another client drives */
	REMOTE_PEER_HANDED,	/* it was given to a project server */
};

struct fyai_remote_peer {
	struct fyai_remote_peer *next;
	struct fyai_remote_server *server;
	struct fyai_remote_session *session;
	struct fyai_remote_run *run;		/* the command the client runs, or NULL */
	struct jsonrpc_conn *conn;
	struct fyai_event_source *deadline;
	unsigned long id;		/* names the client in the log */
	char *dir;			/* the directory of the client, or NULL */
	int fd;
	enum remote_peer_role role;
	bool failed;
	bool native;			/* a fyai session: the client said hello */
	bool lines_only;		/* its transport carries lines: no image mode */
	struct fyai_remote_peer *wnext;	/* the next watcher of the session */
	/* A supervisor that sent a request: it watches only for its answers. */
	bool quiet;
	/* A supervisor that stays: it gets the changes of the state of the
	 * session as fyai session_state frames (doc/fyai-fleet-sdd.md, 7). */
	bool states;
	/* It goes once its output went out: the last frame reaches it. */
	bool drain;
};

struct fyai_remote_session {
	struct fyai_remote_session *next;
	struct fyai_remote_server *server;
	struct fyai_remote_peer *peer;
	struct fyai_remote_peer *watchers;
	/* The snapshot of the UI model that answers an initialize goes to the
	 * client that asked for it alone. */
	struct fyai_remote_peer *snapshot_peer;
	struct jsonrpc_conn *worker;
	int fd;
	pid_t pid;
	bool snapshot;
	struct fyai_event_source *child;
	bool exited;
	/* A session that no branch names ends with its client. */
	bool anonymous;
	/* The messages of a client that the worker has not answered with a
	 * result yet: a session with some is running a turn. */
	unsigned due;
	/* A question waits for the user, as the worker last said. */
	bool waiting;
	/* A client asked the server to keep it past its turns: a parked session. */
	bool keep;
	/* A worker that connected to the server: no child of it. Its channel
	 * closing ends the session. */
	bool adopted;
	/* The worker said that a turn runs, and the state that the supervisors
	 * were told last. */
	bool busy;
	const char *state_sent;
	/* The worker went by a crash, not by an end that was asked for. */
	bool crashed;
	/* A registered worker: its own client drives it, and the server lists it
	 * and serves its watchers. The worker says when it is busy. */
	bool driven;
	const char *branch;
};

/* The project server of one project, which a server of projects or the
 * server of a project above it started. */
/*
 * A client that was given to a project server and that the project server has
 * not yet said it received. This server keeps a copy of its descriptor: a
 * socket that no process holds, with only the message that carries it in
 * flight, is closed on some hosts (macOS), and the client sees the end.
 */
struct fyai_remote_limbo {
	struct fyai_remote_limbo *next;
	uint64_t client;
	int fd;
};

struct fyai_remote_project {
	struct fyai_remote_project *next;
	struct fyai_remote_server *server;
	struct fyai_event_source *child;
	struct fyai_event_source *ack;	/* the receipts of the clients */
	struct fyai_remote_limbo *limbo;
	char *root;
	int fd;				/* SOCK_STREAM: a client in each message */
	pid_t pid;
	bool exited;
};

struct fyai_remote_server {
	struct fyai_cmd_call *call;
	struct fyai_remote_peer *peers;
	struct fyai_remote_session *sessions;
	struct fyai_remote_project *projects;
	struct fyai_event_source *listener, *front;
	/* Encodes the frame that goes to several clients, and is reset for
	 * each: the builder of the context lives as long as the server. */
	struct fy_generic_builder *frame_gb;
	char *path;			/* the endpoint, or NULL behind a front */
	/* The record of the live sessions, where the endpoint of the project
	 * would be: behind a front too, so a list finds the sessions. NULL when
	 * the project has a server of its own. */
	char *record;
	char *base;			/* a server of projects: their directory */
	char *root;			/* a project server: the project */
	int fd, front_fd;
	unsigned long clients;
	char token[65];
	struct fyai_remote_http *http;	/* the HTTP listener, or NULL */
	char http_token[65];		/* the bearer token of the HTTP clients */
	bool require_token;
	bool accept_failed;		/* reported once */
	bool stop;
	/* `serve --once`: stop when the sessions that ran have no turn and no
	 * client. @started is set by the first worker. */
	bool once, started;
	/* The record of the live sessions is out of date. */
	bool dirty;
};

static void remote_log(struct fyai_remote_server *server, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* Write a line of the log on the status stream: the time, the process, @fmt. */
static void remote_log(struct fyai_remote_server *server, const char *fmt, ...)
{
	char stamp[32];
	struct tm tm;
	time_t now;
	va_list ap;
	char *text;
	int len;

	now = time(NULL);
	if (!localtime_r(&now, &tm) ||
	    !strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm))
		stamp[0] = '\0';
	va_start(ap, fmt);
	len = vasprintf(&text, fmt, ap);
	va_end(ap);
	if (len < 0)
		return;
	(void)fyai_report(fyai_cmd_call_ctx(server->call), "%s serve[%ld]: %s\n", stamp,
			  (long)getpid(), text);
	free(text);
}

/* How a child ended, for the log. */
static const char *remote_status_text(int status, char *buf, size_t size)
{
	if (WIFEXITED(status))
		snprintf(buf, size, "exited with status %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		snprintf(buf, size, "was killed by signal %d", WTERMSIG(status));
	else
		snprintf(buf, size, "ended");
	return buf;
}

/* A request of a watcher goes to the worker under this prefix and the number
 * of the watcher, so the answer finds its way back. */
#define REMOTE_WATCH_PREFIX FYAI_REMOTE_WATCH_PREFIX

/* The watcher that the request ID @id names, and the ID it gave, or NULL. */
static struct fyai_remote_peer *remote_watcher_of(struct fyai_remote_session *session,
					     const char *id, const char **restp)
{
	struct fyai_remote_peer *watcher;
	unsigned long n;
	size_t len;
	char *end;

	len = strlen(REMOTE_WATCH_PREFIX);
	if (strncmp(id, REMOTE_WATCH_PREFIX, len))
		return NULL;
	n = strtoul(id + len, &end, 10);
	if (*end != ':')
		return NULL;
	for (watcher = session->watchers; watcher; watcher = watcher->wnext) {
		if (watcher->id == n) {
			*restp = end + 1;
			return watcher;
		}
	}
	return NULL;
}

static void remote_peer_send(struct fyai_remote_peer *peer, fy_generic frame)
{
	if (peer && !peer->failed && fyai_remote_send(peer->conn, frame))
		peer->failed = true;
}

/* A frame in each form that a client can read, made when a client needs it. */
struct remote_wire {
	fy_generic frame;
	const char *line;
	void *image;
	size_t size;
};

/* Queue @wire on @peer as fyai_remote_send() does, in the form that it reads. */
static void remote_peer_send_wire(struct fyai_remote_peer *peer,
				  struct remote_wire *wire)
{
	struct fyai_remote_server *server;
	int rc;

	if (!peer || peer->failed)
		return;
	server = peer->server;
	if (jsonrpc_conn_output_size(peer->conn) >= REMOTE_OUTPUT_MAX) {
		peer->failed = true;
		return;
	}
	if (jsonrpc_conn_image(peer->conn)) {
		if (!wire->image)
			wire->image = jsonrpc_image_encode(peer->conn,
							   wire->frame, &wire->size);
		fyai_error_check(fyai_cmd_call_ctx(server->call), wire->image, err,
				 "cannot encode an image for a watcher");
		rc = jsonrpc_conn_send_image(peer->conn, wire->image, wire->size);
	} else {
		if (!wire->line) {
			if (!server->frame_gb)
				server->frame_gb = fy_generic_builder_create(NULL);
			else
				fy_generic_builder_reset(server->frame_gb);
			fyai_error_check(fyai_cmd_call_ctx(server->call), server->frame_gb, err,
					 "cannot make a builder for the frames of watchers");
			wire->line = jsonrpc_frame_encode(server->frame_gb,
							  wire->frame);
		}
		fyai_error_check(fyai_cmd_call_ctx(server->call), wire->line, err,
				 "cannot encode a frame for a watcher");
		rc = jsonrpc_conn_send_encoded(peer->conn, wire->line);
	}
	if (rc)
		peer->failed = true;
	return;
err:
	peer->failed = true;
}

/*
 * Tell the supervisors that stay the state of @session: `running`, `waiting`,
 * `idle` (with @last, the text of the answer of a turn that ended) or
 * `ended`. A state is sent when it changes, an answer each time.
 */
static void remote_states(struct fyai_remote_session *session, const char *state,
			  fy_generic last)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(session->server->call));
	struct fyai_remote_peer *watcher;
	fy_generic frame;

	if (fy_is_invalid(last) && session->state_sent &&
	    !strcmp(session->state_sent, state))
		return;
	session->state_sent = state;
	frame = fy_mapping(gb, "type", "fyai", "subtype", "session_state",
			   "branch", session->branch ? session->branch : "",
			   "state", state);
	if (fy_is_valid(last))
		frame = fy_assoc(gb, frame, "last", last);
	if (session->crashed && !strcmp(state, "ended"))
		frame = fy_assoc(gb, frame, "crashed", true);
	for (watcher = session->watchers; watcher; watcher = watcher->wnext)
		if (watcher->states)
			remote_peer_send(watcher, frame);
}

/* The state of @session as its worker said it last. */
static const char *remote_state_of(const struct fyai_remote_session *session)
{
	return session->waiting ? "waiting" : session->busy ? "running" : "idle";
}

/*
 * The session ended: a supervisor that stays gets `ended` and goes once the
 * frame is out; any other client fails.
 */
static void remote_session_ended(struct fyai_remote_session *session)
{
	struct fyai_remote_peer *watcher;

	remote_states(session, "ended", fy_invalid);
	if (session->peer)
		session->peer->failed = true;
	for (watcher = session->watchers; watcher; watcher = watcher->wnext) {
		if (watcher->states)
			watcher->drain = true;
		else
			watcher->failed = true;
	}
}

/*
 * The state of a registered worker, from its fyai_worker frame: whether it is
 * busy, and whether its client left it to the server (`driven: false`, with
 * `keep` when no turn is in flight).
 */
static void remote_worker_driven(struct fyai_remote_session *session,
				 fy_generic frame)
{
	unsigned due;

	if (!session->driven)
		return;
	due = fy_get(frame, "busy", false) ? 1 : 0;
	if (session->due != due) {
		session->due = due;
		session->server->dirty = true;
	}
	if (fy_get(frame, "driven", true))
		return;
	session->driven = false;
	session->keep = fy_get(frame, "keep", false);
	session->server->dirty = true;
	remote_log(session->server, "worker %ld: its client left; the server "
		   "keeps the session", (long)session->pid);
}

static void remote_worker_frame(struct jsonrpc_conn *conn, fy_generic frame,
				void *data)
{
	struct fyai_remote_session *session = data;
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(session->server->call);
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct fyai_remote_peer *owner, *watcher;
	const char *branch, *id, *rest;
	struct remote_wire wire = { 0 };
	fy_generic response;
	bool begin;
	const char *copy;

	(void)conn;
	if (fy_equal(fy_get(frame, "type", ""), "result") && session->due) {
		session->due--;
		session->server->dirty = true;
	}
	if (fy_equal(fy_get(frame, "type", ""), "result")) {
		session->busy = false;
		remote_states(session, session->waiting ? "waiting" : "idle",
			      fy_get(frame, "result", fy_value(gb, "")));
	}
	/* The registry learns the branch of the worker; no client sees it. */
	if (fy_equal(fy_get(frame, "type", ""), "fyai_worker")) {
		if (session->waiting != fy_get(frame, "waiting", false)) {
			session->waiting = !session->waiting;
			session->server->dirty = true;
		}
		session->busy = fy_get(frame, "busy", session->busy);
		remote_worker_driven(session, frame);
		/* An idle state comes with the answer, in its result frame. */
		if (strcmp(remote_state_of(session), "idle"))
			remote_states(session, remote_state_of(session), fy_invalid);
		branch = fy_get(frame, "branch", (const char *)NULL);
		if (!branch || !fyai_branch_name_ref_valid(branch) ||
		    (session->branch && !strcmp(session->branch, branch)))
			return;
		copy = fyai_ctx_keep_str(ctx, branch);
		if (!copy)
			return;
		session->branch = copy;
		session->server->dirty = true;
		remote_log(session->server, "worker %ld: branch '%s'",
			   (long)session->pid, branch);
		return;
	}
	/* The model of the UI is sent whole to the client that initialized,
	 * between a begin and an end: a text that another client has would
	 * arrive twice. */
	if (fy_equal(fy_get(frame, "type", ""), "fyai") &&
	    fy_equal(fy_get(frame, "subtype", ""), "snapshot")) {
		id = fy_get(frame, "request_id", "");
		begin = fy_equal(fy_get(frame, "phase", ""), "begin");
		owner = remote_watcher_of(session, id, &rest);
		if (begin) {
			session->snapshot_peer = owner ? owner : session->peer;
			session->snapshot = true;
		}
		if (owner)
			frame = fy_assoc(gb, frame, "request_id", rest);
		remote_peer_send(session->snapshot_peer, frame);
		if (!begin) {
			session->snapshot = false;
			session->snapshot_peer = NULL;
		}
		return;
	}
	if (session->snapshot) {
		remote_peer_send(session->snapshot_peer, frame);
		return;
	}
	/* An answer goes to the client that asked. */
	if (fy_equal(fy_get(frame, "type", ""), "control_response")) {
		response = fy_get(frame, "response", fy_invalid);
		id = fy_get(response, "request_id", "");
		watcher = remote_watcher_of(session, id, &rest);
		if (watcher)
			frame = fy_assoc(gb, frame, "response", fy_assoc(gb,
				response, "request_id", rest));
		remote_peer_send(watcher ? watcher : session->peer, frame);
		return;
	}
	/* The same frame goes to the driver and to each watcher: encode it
	 * once. */
	for (watcher = session->watchers; watcher; watcher = watcher->wnext)
		if (!watcher->quiet)
			break;
	if (!watcher) {
		remote_peer_send(session->peer, frame);
		return;
	}
	wire.frame = frame;
	remote_peer_send_wire(session->peer, &wire);
	for (watcher = session->watchers; watcher; watcher = watcher->wnext)
		if (!watcher->quiet)
			remote_peer_send_wire(watcher, &wire);
	if (wire.image)
		jsonrpc_image_free(wire.image);
}

/*
 * Whether a worker went by a crash: a fatal signal of the program, or an exit
 * status that is not the end that the worker reports itself. A kill and an
 * end that was asked for are not crashes.
 */
static bool remote_status_crashed(int status)
{
	int sig;

	if (WIFSIGNALED(status)) {
		sig = WTERMSIG(status);
		return sig == SIGSEGV || sig == SIGABRT || sig == SIGBUS ||
		       sig == SIGFPE || sig == SIGILL;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) &&
	       WEXITSTATUS(status) != FYAI_WORKER_EXIT_ENDED;
}

static enum fyai_event_action remote_worker_exit(const struct fyai_event *ev)
{
	struct fyai_remote_session *session = ev->userdata;
	char how[64];

	session->child = NULL;
	session->exited = true;
	session->crashed = remote_status_crashed(ev->status);
	session->server->dirty = true;
	remote_log(session->server, "worker %ld (branch '%s') %s", (long)ev->pid,
		   session->branch ? session->branch : "none",
		   remote_status_text(ev->status, how, sizeof(how)));
	remote_session_ended(session);
	return FYAIEA_CONTINUE;
}

/*
 * In the child of a worker: put the channel on standard input and output and
 * the credential transport on fixed descriptors, close every other descriptor,
 * change to the directory @dir of the client and execute fyai. Each descriptor
 * moves above the fixed ones first, so no dup2() replaces one that is still to
 * move.
 */
static void remote_worker_exec(struct fyai_ctx *ctx, const char *self,
			       char **args, const char *dir, int channel,
			       int agent, int ctl, int sync)
{
	int high = 64;

	if (dir && chdir(dir))
		_exit(126);
	channel = fcntl(channel, F_DUPFD, high);
	if (agent >= 0) {
		agent = fcntl(agent, F_DUPFD, high);
		ctl = fcntl(ctl, F_DUPFD, high);
		sync = fcntl(sync, F_DUPFD, high);
		if (agent < 0 || ctl < 0 || sync < 0)
			_exit(126);
	}
	if (channel < 0 || dup2(channel, STDIN_FILENO) < 0 ||
	    dup2(channel, STDOUT_FILENO) < 0)
		_exit(126);
	if (agent >= 0 &&
	    (dup2(agent, REMOTE_WORKER_AGENT_FD) < 0 ||
	     dup2(ctl, REMOTE_WORKER_CTL_FD) < 0 ||
	     dup2(sync, REMOTE_WORKER_SYNC_FD) < 0))
		_exit(126);
	fyai_close_fds_from(agent >= 0 ? REMOTE_WORKER_SYNC_FD + 1 :
			    STDERR_FILENO + 1);
	if (agent >= 0) {
		if (fyai_transport_worker_attach(REMOTE_WORKER_SYNC_FD,
						 REMOTE_WORKER_AGENT_FD,
						 REMOTE_WORKER_CTL_FD))
			_exit(126);
		close(REMOTE_WORKER_SYNC_FD);
	}
	fyai_loop_restore_signal_mask(ctx->loop);
	signal(SIGPIPE, SIG_DFL);
	execvp(self, args);
	_exit(127);
}

/*
 * The command line of a child of the server: the executable and the global
 * options this invocation had, with room for @extra more and a NULL. The
 * executable is resolved to a path into *@selfp, which the caller frees with
 * the vector.
 */
static char **remote_child_args(struct fyai_ctx *ctx, size_t extra,
				size_t *countp, char **selfp)
{
	char *const *original = ctx->cfg->argv;
	char **args;
	size_t n;

	*selfp = NULL;
	for (n = 0; original[n] && strcmp(original[n], "serve"); n++)
		;
	fyai_error_check(ctx, original[n], err,
			 "cannot find the serve command in the arguments");
	*selfp = realpath(original[0], NULL);
	fyai_error_check(ctx, *selfp || !strchr(original[0], '/'), err,
			 "cannot resolve the executable '%s': %s", original[0],
			 strerror(errno));
	args = calloc(n + extra + 1, sizeof(*args));
	fyai_error_check(ctx, args, err, "cannot allocate the arguments of a child");
	memcpy(args, original, n * sizeof(*args));
	if (*selfp)
		args[0] = *selfp;
	*countp = n;
	return args;
err:
	free(*selfp);
	*selfp = NULL;
	return NULL;
}

/*
 * Start a worker for @peer on @branch, or on a new branch for NULL, with the
 * global options @words of the client after those of this invocation.
 */
static int remote_spawn(struct fyai_remote_peer *peer, const char *branch,
			fy_generic words)
{
	struct fyai_remote_server *server = peer->server;
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	struct fyai_remote_session *session = NULL;
	char **args = NULL, *self = NULL;
	int channel[2] = { -1, -1 }, agent[2] = { -1, -1 };
	int ctl[2] = { -1, -1 }, sync[2] = { -1, -1 };
	int i, rc = -1;
	const char *word;
	size_t n;
	pid_t pid = -1;

	/* The worker runs with the global options this invocation had. */
	args = remote_child_args(ctx, 6 + fy_len(words), &n, &self);
	if (!args)
		goto out;
	session = calloc(1, sizeof(*session));
	fyai_error_check(ctx, session, out, "cannot allocate a session worker");
	fy_foreach(word, words) {
		args[n] = (char *)word;
		n++;
	}
	if (branch) {
		args[n++] = "--branch";
		args[n++] = (char *)branch;
	}
	args[n++] = "claude-sdk";
	args[n++] = "--remote-worker";
	args[n++] = "--session-type";
	args[n] = peer->native ? "fyai" : "sdk";
	rc = fyai_remote_socketpair(SOCK_STREAM, channel);
	fyai_error_check(ctx, !rc, out, "cannot make the channel of a "
			 "worker: %s", strerror(errno));
	if (ctx->transport->client) {
		rc = fyai_remote_socketpair(SOCK_SEQPACKET, agent);
		if (!rc)
			rc = fyai_remote_socketpair(SOCK_SEQPACKET, ctl);
		if (!rc)
			rc = pipe(sync);
		if (!rc)
			rc = fyai_remote_cloexec(sync[0]) || fyai_remote_cloexec(sync[1]);
		fyai_error_check(ctx, !rc, out, "cannot make the transport "
				 "channels of a worker: %s", strerror(errno));
	}
	rc = -1;
	pid = fork();
	fyai_error_check(ctx, pid >= 0, out, "cannot fork a worker: %s",
			 strerror(errno));
	if (!pid)
		remote_worker_exec(ctx, args[0], args, peer->dir, channel[1],
				   agent[0], ctl[0], sync[0]);
	if (ctx->transport->client) {
		rc = fyai_transport_worker_admit(ctx->transport, pid, agent[1], ctl[1],
						 sync[1]);
		fyai_error_check(ctx, !rc, out,
				 "cannot admit a worker to the transport");
		rc = -1;
	}
	session->branch = fyai_ctx_keep_str(ctx, branch);
	fyai_error_check(ctx, !branch || session->branch, out,
			 "cannot keep the branch of a worker");
	session->anonymous = !branch;
	session->server = server;
	session->peer = peer;
	session->pid = pid;
	session->fd = channel[0];
	session->worker = jsonrpc_conn_socket(ctx, channel[0], 0,
					      "remote worker", NULL);
	fyai_error_check(ctx, session->worker, out,
			 "cannot open the channel of a worker");
	jsonrpc_conn_expect_close(session->worker);
	/* Both ends are this program: the worker sets the same mode. */
	(void)jsonrpc_conn_set_image(session->worker);
	rc = jsonrpc_conn_frames(session->worker, remote_worker_frame,
				 session);
	fyai_error_check(ctx, !rc, out, "cannot read the channel of a worker");
	rc = fyai_event_add_child(fyai_ctx_loop(ctx), pid, remote_worker_exit,
				  session, &session->child);
	fyai_error_check(ctx, !rc, out, "cannot watch a worker");
	channel[0] = -1;
	session->next = server->sessions;
	server->sessions = session;
	peer->session = session;
	session = NULL;
	server->started = true;
	server->dirty = true;
	remote_log(server, "client %lu: started the worker %ld on %s%s%s", peer->id,
		   (long)pid, branch ? "branch '" : "a new branch",
		   branch ? branch : "", branch ? "'" : "");
	rc = 0;
out:
	for (i = 0; i < 2; i++) {
		if (channel[i] >= 0)
			close(channel[i]);
		if (agent[i] >= 0)
			close(agent[i]);
		if (ctl[i] >= 0)
			close(ctl[i]);
		if (sync[i] >= 0)
			close(sync[i]);
	}
	if (session) {
		jsonrpc_conn_destroy(session->worker);
		free(session);
	}
	if (rc && pid > 0)
		fyai_event_child_terminate(fyai_ctx_loop(ctx), pid, 0, 1000,
					   NULL);
	free(args);
	free(self);
	return rc;
}

/*
 * A command that a client runs with --remote: fyai --sink-frames in the
 * directory of the client. Its frames come on its standard output, which
 * goes to the client as they come, its standard error is sent as text, and
 * the standard input of the client goes to it. When it ended and both of
 * its outputs closed, the client gets its exit status.
 */
struct fyai_remote_run {
	struct fyai_remote_peer *peer;
	struct jsonrpc_conn *out;
	struct fyai_event_source *child, *err_src, *in_src;
	char *name;			/* the first words of the command, for the log */
	char *input;			/* input that the command did not read yet */
	size_t input_len;
	int out_fd, err_fd, in_fd;
	int status;
	pid_t pid;
	bool exited, err_closed, input_closed, reported;
};

static void remote_run_frame(struct jsonrpc_conn *conn, fy_generic frame,
			     void *data)
{
	struct fyai_remote_run *run = data;

	(void)conn;
	if (run->peer && !run->peer->failed && fyai_remote_send(run->peer->conn, frame))
		run->peer->failed = true;
}

static enum fyai_event_action remote_run_stderr(const struct fyai_event *ev)
{
	struct fyai_remote_run *run = ev->userdata;
	struct fy_generic_builder *gb;
	char buf[4096];
	ssize_t n;

	n = read(run->err_fd, buf, sizeof(buf));
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return FYAIEA_CONTINUE;
	if (n <= 0) {
		run->err_closed = true;
		run->err_src = NULL;
		fyai_event_source_remove(ev->src);
		return FYAIEA_CONTINUE;
	}
	if (run->peer && !run->peer->failed) {
		gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(run->peer->server->call));
		if (fyai_remote_send(run->peer->conn, fy_mapping(gb, "type", "fyai",
				"subtype", "stderr",
				"data", fy_gb_string_size_create(gb, buf, (size_t)n))))
			run->peer->failed = true;
	}
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action remote_run_exit(const struct fyai_event *ev)
{
	struct fyai_remote_run *run = ev->userdata;

	run->child = NULL;
	run->exited = true;
	run->status = ev->status;
	return FYAIEA_CONTINUE;
}

/* Write what waits of the input of the client to the command. */
static enum fyai_event_action remote_run_input(const struct fyai_event *ev)
{
	struct fyai_remote_run *run = ev->userdata;
	ssize_t n;

	while (run->input_len) {
		n = write(run->in_fd, run->input, run->input_len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return FYAIEA_CONTINUE;
		if (n <= 0) {
			/* A command that closed its input reads no more. */
			run->input_len = 0;
			run->input_closed = true;
			break;
		}
		memmove(run->input, run->input + n, run->input_len - (size_t)n);
		run->input_len -= (size_t)n;
	}
	/* Withdraw the source before the descriptor closes: a closed
	 * descriptor leaves the registration in the kernel while a forked
	 * child holds a copy, and the loop then reads a freed source. */
	run->in_src = NULL;
	fyai_event_source_remove(ev->src);
	if (run->input_closed && run->in_fd >= 0) {
		close(run->in_fd);
		run->in_fd = -1;
	}
	return FYAIEA_CONTINUE;
}

/* Keep @len bytes of the input of the client for the command. */
static int remote_run_feed(struct fyai_remote_run *run, const char *data, size_t len,
			   bool closed)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(run->peer->server->call);
	char *grown;
	int rc;

	if (run->in_fd < 0)
		return 0;
	if (len) {
		grown = realloc(run->input, run->input_len + len);
		fyai_error_check(ctx, grown, err, "cannot keep the input of a command");
		run->input = grown;
		memcpy(run->input + run->input_len, data, len);
		run->input_len += len;
	}
	if (closed)
		run->input_closed = true;
	if (!run->in_src) {
		rc = fyai_event_add_fd(fyai_ctx_loop(ctx), run->in_fd, FYAIEV_WRITE,
				       remote_run_input, run, &run->in_src);
		fyai_error_check(ctx, !rc, err, "cannot write the input of a command");
	}
	return 0;
err:
	return -1;
}

static void remote_run_free(struct fyai_remote_run *run)
{
	struct fyai_event_loop *loop;

	if (!run)
		return;
	loop = fyai_ctx_loop(fyai_cmd_call_ctx(run->peer->server->call));
	fyai_event_source_remove(run->in_src);
	fyai_event_source_remove(run->err_src);
	jsonrpc_conn_destroy(run->out);
	if (run->out_fd >= 0)
		close(run->out_fd);
	if (run->err_fd >= 0)
		close(run->err_fd);
	if (run->in_fd >= 0)
		close(run->in_fd);
	/* A command of a client that went ends with it. */
	if (!run->exited) {
		fyai_event_source_remove(run->child);
		(void)fyai_event_add_child_terminate(loop, run->pid, 0, 1000, NULL,
						     NULL, NULL);
	}
	free(run->input);
	free(run->name);
	free(run);
}

/* In the child of a command: its descriptors, its directory, and fyai. */
static void remote_run_exec(struct fyai_ctx *ctx, char **args, const char *dir,
			    int out, int err, int in, long cols)
{
	char text[32];
	int null;

	if (dir && chdir(dir))
		_exit(126);
	out = fcntl(out, F_DUPFD, 64);
	err = fcntl(err, F_DUPFD, 64);
	if (in >= 0) {
		in = fcntl(in, F_DUPFD, 64);
	} else {
		null = open("/dev/null", O_RDONLY);
		in = null >= 0 ? fcntl(null, F_DUPFD, 64) : -1;
	}
	if (out < 0 || err < 0 || in < 0 || dup2(in, STDIN_FILENO) < 0 ||
	    dup2(out, STDOUT_FILENO) < 0 || dup2(err, STDERR_FILENO) < 0)
		_exit(126);
	fyai_close_fds_from(STDERR_FILENO + 1);
	/* The client renders, at its own width. */
	if (cols > 0) {
		snprintf(text, sizeof(text), "%ld", cols);
		setenv("COLUMNS", text, 1);
	}
	fyai_loop_restore_signal_mask(ctx->loop);
	signal(SIGPIPE, SIG_DFL);
	execvp(args[0], args);
	_exit(127);
}

/* Run the command line @words for @peer, with its input when @input. */
static int remote_run_start(struct fyai_remote_peer *peer, fy_generic words,
			    bool input, long cols)
{
	struct fyai_remote_server *server = peer->server;
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	struct fyai_remote_run *run;
	int out[2] = { -1, -1 }, err[2] = { -1, -1 }, in[2] = { -1, -1 };
	char **args = NULL, *self = NULL;
	size_t i, n = 0, count = fy_len(words);
	pid_t pid;
	int rc;

	run = calloc(1, sizeof(*run));
	fyai_error_check(ctx, run, fail, "cannot allocate a command");
	run->peer = peer;
	run->out_fd = run->err_fd = run->in_fd = -1;
	run->status = -1;
	self = realpath(ctx->cfg->argv[0], NULL);
	args = calloc(count + 3, sizeof(*args));
	fyai_error_check(ctx, args, fail, "cannot allocate the arguments of a command");
	args[n++] = self ? self : ctx->cfg->argv[0];
	args[n++] = "--sink-frames";
	for (i = 0; i < count; i++) {
		args[n] = (char *)fy_get_at(words, i, (const char *)NULL);
		fyai_error_check(ctx, args[n], fail, "a word of the command is not a string");
		n++;
	}
	run->name = strdup(count ? args[2] : "(interactive)");
	rc = fyai_remote_socketpair(SOCK_STREAM, out);
	if (!rc)
		rc = pipe(err);
	if (!rc)
		rc = fyai_remote_cloexec(err[0]) || fyai_remote_cloexec(err[1]);
	if (!rc && input)
		rc = pipe(in);
	if (!rc && input)
		rc = fyai_remote_cloexec(in[0]) || fyai_remote_cloexec(in[1]);
	fyai_error_check(ctx, !rc && run->name, fail, "cannot make the channels of a "
			 "command: %s", strerror(errno));
	pid = fork();
	fyai_error_check(ctx, pid >= 0, fail, "cannot fork a command: %s",
			 strerror(errno));
	if (!pid)
		remote_run_exec(ctx, args, peer->dir, out[1], err[1], in[0], cols);
	run->pid = pid;
	close(out[1]);
	close(err[1]);
	out[1] = err[1] = -1;
	if (in[0] >= 0) {
		close(in[0]);
		in[0] = -1;
	}
	run->out_fd = out[0];
	run->err_fd = err[0];
	run->in_fd = in[1];
	out[0] = err[0] = in[1] = -1;
	rc = fyai_event_add_child(fyai_ctx_loop(ctx), pid, remote_run_exit, run,
				  &run->child);
	fyai_error_check(ctx, !rc, fail_run, "cannot watch a command");
	rc = fcntl(run->err_fd, F_SETFL, fcntl(run->err_fd, F_GETFL) | O_NONBLOCK) < 0;
	if (!rc && run->in_fd >= 0)
		rc = fcntl(run->in_fd, F_SETFL,
			   fcntl(run->in_fd, F_GETFL) | O_NONBLOCK) < 0;
	if (!rc)
		rc = fyai_event_add_fd(fyai_ctx_loop(ctx), run->err_fd, FYAIEV_READ,
				       remote_run_stderr, run, &run->err_src);
	fyai_error_check(ctx, !rc, fail_run, "cannot read the standard error of a "
			 "command");
	run->out = jsonrpc_conn_socket(ctx, run->out_fd, 0, "remote command", NULL);
	fyai_error_check(ctx, run->out, fail_run, "cannot open the output of a command");
	jsonrpc_conn_expect_close(run->out);
	rc = jsonrpc_conn_frames(run->out, remote_run_frame, run);
	fyai_error_check(ctx, !rc, fail_run, "cannot read the output of a command");
	peer->run = run;
	remote_log(server, "client %lu: started the command %ld: %s", peer->id,
		   (long)pid, run->name);
	free(args);
	free(self);
	return 0;
fail_run:
	remote_run_free(run);
	run = NULL;
fail:
	for (i = 0; i < 2; i++) {
		if (out[i] >= 0)
			close(out[i]);
		if (err[i] >= 0)
			close(err[i]);
		if (in[i] >= 0)
			close(in[i]);
	}
	if (run) {
		free(run->name);
		free(run);
	}
	free(args);
	free(self);
	return -1;
}

/* When the command of @peer ended and its outputs closed, send its status. */
static void remote_run_settle(struct fyai_remote_peer *peer)
{
	struct fyai_remote_run *run = peer->run;
	struct fy_generic_builder *gb;
	char how[64];
	int code;

	if (!run || run->reported || !run->exited || !run->err_closed ||
	    !jsonrpc_conn_closed(run->out))
		return;
	run->reported = true;
	code = WIFEXITED(run->status) ? WEXITSTATUS(run->status) :
	       128 + (WIFSIGNALED(run->status) ? WTERMSIG(run->status) : 0);
	remote_log(peer->server, "client %lu: the command %ld %s", peer->id,
		   (long)run->pid, remote_status_text(run->status, how, sizeof(how)));
	gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(peer->server->call));
	if (fyai_remote_send(peer->conn, fy_mapping(gb, "type", "fyai", "subtype", "exit",
					       "status", code)))
		peer->failed = true;
}

/* A frame of a client that runs a command: its input, or a signal. */
static void remote_run_client(struct fyai_remote_peer *peer, fy_generic frame)
{
	struct fyai_remote_run *run = peer->run;
	fy_generic data = fy_get(frame, "data", fy_invalid);
	const char *subtype = fy_get(frame, "subtype", ""), *name;
	int signo = 0;

	if (!fy_equal(fy_get(frame, "type", ""), "fyai")) {
		peer->failed = true;
		return;
	}
	if (!strcmp(subtype, "stdin")) {
		if (remote_run_feed(run, fy_is_string(data) ? fy_castp(&data, "") : "",
				    fy_is_string(data) ? fy_len(data) : 0,
				    fy_get(frame, "closed", false)))
			peer->failed = true;
		return;
	}
	if (!strcmp(subtype, "signal")) {
		name = fy_get(frame, "signal", "");
		if (!strcmp(name, "INT"))
			signo = SIGINT;
		else if (!strcmp(name, "TERM"))
			signo = SIGTERM;
		else if (!strcmp(name, "HUP"))
			signo = SIGHUP;
		if (signo && !run->exited)
			kill(run->pid, signo);
		return;
	}
	peer->failed = true;
}

static int remote_reply(struct fyai_remote_peer *peer, fy_generic id,
			const char *error, fy_generic response)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(peer->server->call));
	fy_generic value;
	int rc;

	value = fy_mapping(gb, "subtype", error ? "error" : "success",
			   "request_id", id);
	value = fy_assoc(gb, value, error ? "error" : "response",
			 error ? fy_value(gb, error) : response);
	rc = fyai_remote_send(peer->conn, fy_mapping(gb, "type", "control_response",
						"response", value));
	if (rc)
		peer->failed = true;
	return rc;
}

/*
 * A frame of a watcher: it may ask the worker for the model of the UI and for
 * stored turns, and nothing else. Its IDs get the prefix that routes the
 * answer.
 */
static void remote_watch_request(struct fyai_remote_peer *peer, fy_generic frame,
				 fy_generic id, const char *subtype)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(peer->server->call));
	struct fyai_remote_session *session = peer->session;
	fy_generic request;
	const char *text;
	bool send;

	if (!fy_is_string(id)) {
		peer->failed = true;
		return;
	}
	send = !strcmp(subtype, "fyai_session_send");
	if (strcmp(subtype, "initialize") && strcmp(subtype, "fyai_sync") &&
	    strcmp(subtype, "fyai_query") && !send) {
		remote_reply(peer, id, "a watcher only reads", fy_invalid);
		return;
	}
	if (!session->worker) {
		peer->failed = true;
		return;
	}
	text = fy_castp(&id, "");
	/* The lines that a watcher types are messages to the session; it stays a
	 * watcher that gets every frame. */
	if (send) {
		request = fy_get(frame, "request", fy_invalid);
		frame = fy_mapping(gb, "type", "control_request", "request", fy_mapping(gb,
			"subtype", "fyai_message", "text", fy_get(request, "text", ""),
			"from", fy_get(request, "from", "a watcher")));
		remote_log(peer->server, "client %lu: sends a message to the worker %ld "
			   "on branch '%s'", peer->id, (long)session->pid,
			   session->branch ? session->branch : "none");
	}
	frame = fy_assoc(gb, frame, "request_id", fy_stringf(gb,
		REMOTE_WATCH_PREFIX "%lu:%s", peer->id, text));
	if (fyai_remote_send(session->worker, frame))
		peer->failed = true;
}

/* The first frame of a peer that needs a token: the token, or nothing. */
static void remote_authenticate(struct fyai_remote_peer *peer, fy_generic frame)
{
	struct fyai_remote_server *server = peer->server;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	fy_generic request, token;
	const char *text;

	request = fy_get(frame, "request", fy_invalid);
	token = fy_get(request, "token", fy_invalid);
	text = fy_castp(&token, "");
	if (!fy_equal(fy_get(frame, "type", ""), "control_request") ||
	    !fy_equal(fy_get(frame, "request_id", ""), "auth") ||
	    !fy_equal(fy_get(request, "subtype", ""), "fyai_authenticate") ||
	    strlen(text) != 64 || CRYPTO_memcmp(text, server->token, 64)) {
		remote_log(server, "client %lu: refused: no valid launch token",
			   peer->id);
		peer->failed = true;
		return;
	}
	peer->role = REMOTE_PEER_CLIENT;
	fyai_event_source_remove(peer->deadline);
	peer->deadline = NULL;
	if (jsonrpc_conn_send_frame(peer->conn, fy_mapping(gb,
	    "type", "control_response", "response", fy_mapping(gb,
	    "subtype", "success", "request_id", "auth",
	    "response", fy_map_empty))))
		peer->failed = true;
}

/*
 * Attach @peer to the live session of @branch, or start one there with the
 * global options @words.
 */
static void remote_attach(struct fyai_remote_peer *peer, fy_generic id,
			  const char *branch, fy_generic words, bool watch,
			  bool supervise)
{
	struct fyai_remote_server *server = peer->server;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	struct fyai_remote_session *session;

	for (session = server->sessions; session; session = session->next)
		if (!session->exited && session->branch &&
		    !strcmp(session->branch, branch))
			break;
	if (watch) {
		/* A watcher reads a session that is running: it starts none and
		 * takes nothing from the client that drives it. */
		if (!session || peer->session) {
			remote_log(server, "client %lu: refused: no session of '%s'"
				   "to watch", peer->id, branch);
			remote_reply(peer, id, session ? "the client has a session" :
				     "no live session on the branch", fy_invalid);
			return;
		}
		peer->role = REMOTE_PEER_WATCHER;
		peer->session = session;
		peer->wnext = session->watchers;
		session->watchers = peer;
		/* A supervisor that stays gets the state, not the frames, and the
		 * state that the session has now. */
		peer->quiet = peer->states = supervise;
		remote_log(server, "client %lu: %s the worker %ld on branch '%s'",
			   peer->id, supervise ? "supervises" : "watches",
			   (long)session->pid, branch);
		remote_reply(peer, id, NULL, fy_mapping(gb, "branch", branch,
				"state", remote_state_of(session)));
		return;
	}
	if (session && ((session->peer && session->peer != peer) ||
			session->driven)) {
		remote_log(server, "client %lu: refused: the session of '%s' has "
			   "a client", peer->id, branch);
		remote_reply(peer, id, "the session has a client", fy_invalid);
		return;
	}
	if (peer->session)
		peer->session->peer = NULL;
	peer->session = session;
	server->dirty = true;
	if (session) {
		session->peer = peer;
		remote_log(server, "client %lu: attached to the worker %ld on "
			   "branch '%s'", peer->id, (long)session->pid, branch);
	} else if (remote_spawn(peer, branch, words)) {
		remote_reply(peer, id, "cannot start a session worker",
			     fy_invalid);
		return;
	}
	remote_reply(peer, id, NULL, fy_mapping(gb, "branch", branch));
}

/* True when @path is @dir or below it. */
static bool remote_path_within(const char *dir, const char *path)
{
	size_t len = strlen(dir);

	if (len == 1 && dir[0] == '/')
		return path[0] == '/';
	return !strncmp(dir, path, len) && (!path[len] || path[len] == '/');
}

/*
 * The directory that a client names, @path, relative to the directory that the
 * server serves, and the project root that holds it: *@dirp and *@rootp, which
 * the caller frees. A server of projects serves only directories under its
 * own, and a project server only its project and the projects below it: the
 * nearest project that holds the directory (fyai_fleet_project_of()). NULL
 * with the reason in *@why.
 */
static int remote_resolve(struct fyai_remote_server *server, const char *path,
			  char **dirp, char **rootp, const char **why)
{
	const char *base = server->base ? server->base : server->root;
	char *joined = NULL, *dir = NULL, *root = NULL;
	struct stat st;
	int rc;

	*dirp = *rootp = NULL;
	if (path[0] == '/') {
		joined = strdup(path);
		rc = joined ? 0 : -1;
	} else {
		rc = asprintf(&joined, "%s/%s", base, path);
	}
	if (rc < 0) {
		*why = "cannot allocate the directory";
		return -1;
	}
	dir = realpath(joined, NULL);
	free(joined);
	if (!dir || stat(dir, &st) || !S_ISDIR(st.st_mode)) {
		*why = "the directory does not exist";
		goto err;
	}
	if (!remote_path_within(base, dir)) {
		*why = server->base ? "the directory is not under the projects of "
			"the server" : "the directory is not in the project of the "
			"server";
		goto err;
	}
	root = server->base ? fyai_discover_project_root_from(dir) :
		fyai_fleet_project_of(dir);
	/* A project that is the directory of the projects is not served, and a
	 * project server serves no project above its own. */
	if (root && !remote_path_within(base, root)) {
		free(root);
		root = NULL;
	}
	if (!root)
		root = strdup(server->base ? dir : server->root);
	if (!root) {
		*why = "cannot allocate the directory";
		goto err;
	}
	*dirp = dir;
	*rootp = root;
	return 0;
err:
	free(dir);
	return -1;
}

static enum fyai_event_action remote_project_exit(const struct fyai_event *ev)
{
	struct fyai_remote_project *project = ev->userdata;
	char how[64];

	project->child = NULL;
	project->exited = true;
	remote_log(project->server, "project server %ld for '%s' %s",
		   (long)ev->pid, project->root,
		   remote_status_text(ev->status, how, sizeof(how)));
	return FYAIEA_CONTINUE;
}

/* Close the copies of the clients that @project received, or all of them. */
static void remote_limbo_close(struct fyai_remote_project *project, uint64_t client,
			       bool all)
{
	struct fyai_remote_limbo **link = &project->limbo, *l;

	while ((l = *link)) {
		if (!all && l->client != client) {
			link = &l->next;
			continue;
		}
		*link = l->next;
		close(l->fd);
		free(l);
	}
}

/* A project server says which client it received: this server lets it go. */
static enum fyai_event_action remote_project_ack(const struct fyai_event *ev)
{
	struct fyai_remote_project *project = ev->userdata;
	uint64_t client;
	ssize_t n;

	for (;;) {
		do {
			n = recv(ev->fd, &client, sizeof(client), MSG_DONTWAIT);
		} while (n < 0 && errno == EINTR);
		if (n == (ssize_t)sizeof(client)) {
			remote_limbo_close(project, client, false);
			continue;
		}
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			/* The project server is gone: nothing is in flight. */
			fyai_event_source_remove(project->ack);
			project->ack = NULL;
			remote_limbo_close(project, 0, true);
		}
		return FYAIEA_CONTINUE;
	}
}

static void remote_project_free(struct fyai_remote_project *project)
{
	struct fyai_event_loop *loop = fyai_ctx_loop(fyai_cmd_call_ctx(project->server->call));

	fyai_event_source_remove(project->ack);
	remote_limbo_close(project, 0, true);
	if (project->fd >= 0)
		close(project->fd);
	fyai_event_source_remove(project->child);
	if (!project->exited)
		fyai_event_child_terminate(loop, project->pid, 0, 1000, NULL);
	free(project->root);
	free(project);
}

/*
 * Start the project server of @root: fyai serve in that directory, which takes
 * its clients from this process on REMOTE_FRONT_FD. It has the global options
 * of this invocation, and it writes to the same log.
 */
static struct fyai_remote_project *remote_project_start(struct fyai_remote_server *server,
						   const char *root)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	struct fyai_remote_project *project = NULL;
	char **args = NULL, *self = NULL;
	int channel[2] = { -1, -1 }, fd, rc;
	size_t n;
	pid_t pid = -1;

	args = remote_child_args(ctx, 4, &n, &self);
	if (!args)
		goto err;
	args[n++] = "serve";
	args[n++] = "start";
	args[n++] = "--front-fd";
	args[n] = "3";
	project = calloc(1, sizeof(*project));
	fyai_error_check(ctx, project, err, "cannot allocate a project server");
	project->fd = -1;
	project->root = strdup(root);
	fyai_error_check(ctx, project->root, err, "cannot keep the project '%s'", root);
	rc = fyai_remote_socketpair(SOCK_STREAM, channel);
	fyai_error_check(ctx, !rc, err, "cannot make the channel of a project "
			 "server: %s", strerror(errno));
	pid = fork();
	fyai_error_check(ctx, pid >= 0, err, "cannot fork a project server: %s",
			 strerror(errno));
	if (!pid) {
		fd = fcntl(channel[1], F_DUPFD, 64);
		if (fd < 0 || dup2(fd, REMOTE_FRONT_FD) < 0 || chdir(root))
			_exit(126);
		fyai_close_fds_from(REMOTE_FRONT_FD + 1);
		fyai_loop_restore_signal_mask(ctx->loop);
		signal(SIGPIPE, SIG_DFL);
		execvp(args[0], args);
		_exit(127);
	}
	close(channel[1]);
	channel[1] = -1;
	project->server = server;
	project->pid = pid;
	project->fd = channel[0];
	channel[0] = -1;
	rc = fyai_event_add_child(fyai_ctx_loop(ctx), pid, remote_project_exit,
				  project, &project->child);
	fyai_error_check(ctx, !rc, err, "cannot watch a project server");
	rc = fyai_event_add_fd(fyai_ctx_loop(ctx), project->fd, FYAIEV_READ,
			       remote_project_ack, project, &project->ack);
	fyai_error_check(ctx, !rc, err, "cannot read the receipts of a project "
			 "server");
	project->next = server->projects;
	server->projects = project;
	remote_log(server, "started the project server %ld for '%s'", (long)pid,
		   root);
	free(args);
	free(self);
	return project;
err:
	if (channel[0] >= 0)
		close(channel[0]);
	if (channel[1] >= 0)
		close(channel[1]);
	if (project) {
		project->server = server;
		if (pid > 0 && !project->child)
			fyai_event_child_terminate(fyai_ctx_loop(ctx), pid, 0, 1000,
						   NULL);
		project->exited = true;
		remote_project_free(project);
	}
	free(args);
	free(self);
	return NULL;
}

/* Send @text and the descriptor @fd as one message on @sock. */
static int remote_send_fd(int sock, const char *text, int fd)
{
	char control[CMSG_SPACE(sizeof(int))] = { 0 };
	uint32_t len = (uint32_t)strlen(text);
	struct iovec iov[2] = {
		{ .iov_base = &len, .iov_len = sizeof(len) },
		{ .iov_base = (void *)text, .iov_len = len },
	};
	struct msghdr msg = {
		.msg_iov = iov, .msg_iovlen = 2,
		.msg_control = control, .msg_controllen = sizeof(control),
	};
	struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
	ssize_t n;

	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &fd, sizeof(int));
	do {
		n = sendmsg(sock, &msg, MSG_NOSIGNAL);
	} while (n < 0 && errno == EINTR);
	return n == (ssize_t)(sizeof(len) + len) ? 0 : -1;
}

/* Read exactly @size bytes from the stream @sock; 0, or -1 with errno set. */
static int remote_read_exact(int sock, char *buf, size_t size)
{
	struct pollfd pfd = { .fd = sock, .events = POLLIN };
	ssize_t n;

	while (size) {
		n = recv(sock, buf, size, 0);
		if (n > 0) {
			buf += n;
			size -= (size_t)n;
			continue;
		}
		if (!n) {
			errno = ECONNRESET;
			return -1;
		}
		if (errno == EINTR)
			continue;
		/* The sender writes a message with one call: the rest of it
		 * is already queued or arrives at once. */
		if (errno != EAGAIN || poll(&pfd, 1, REMOTE_ADMIT_MS) <= 0)
			return -1;
	}
	return 0;
}

/*
 * Receive a message and its descriptor from the stream @sock. A message is
 * its length as a 32-bit word and its text, sent with the descriptor.
 * Return the length of the text in @buf and the descriptor in *@fdp, 0 when
 * the sender closed, or -1.
 */
static ssize_t remote_recv_fd(int sock, char *buf, size_t size, int *fdp)
{
	char control[CMSG_SPACE(sizeof(int))];
	uint32_t len;
	struct iovec iov = { .iov_base = &len, .iov_len = sizeof(len) };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control, .msg_controllen = sizeof(control),
	};
	struct cmsghdr *c;
	ssize_t n;

	*fdp = -1;
	do {
		n = recvmsg(sock, &msg, REMOTE_RECV_FLAGS);
	} while (n < 0 && errno == EINTR);
	if (n <= 0)
		return n;
	for (c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
		    c->cmsg_len == CMSG_LEN(sizeof(int)))
			memcpy(fdp, CMSG_DATA(c), sizeof(int));
	if (*fdp < 0 || (msg.msg_flags & MSG_CTRUNC) || fyai_remote_cloexec(*fdp))
		goto bad;
	if ((size_t)n < sizeof(len) &&
	    remote_read_exact(sock, (char *)&len + n, sizeof(len) - (size_t)n))
		goto fail;
	if (!len || len >= size)
		goto bad;
	if (remote_read_exact(sock, buf, len))
		goto fail;
	buf[len] = '\0';
	return len;
bad:
	errno = EPROTO;
fail:
	if (*fdp >= 0)
		close(*fdp);
	*fdp = -1;
	return -1;
}

/*
 * Give @peer, which asked for @dir in @root, to the project server of @root,
 * which this server starts when it does not run. That server answers the
 * request @id.
 */
static void remote_hand_over(struct fyai_remote_peer *peer, fy_generic id,
			     const char *dir, const char *root)
{
	struct fyai_remote_server *server = peer->server;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	struct fyai_remote_project *project;
	struct fyai_remote_limbo *limbo;
	const char *text;

	for (project = server->projects; project; project = project->next)
		if (!project->exited && !strcmp(project->root, root))
			break;
	if (!project)
		project = remote_project_start(server, root);
	if (!project) {
		remote_reply(peer, id, "cannot start the project server", fy_invalid);
		return;
	}
	text = emit_json_string(gb, fy_mapping(gb, "client", (long long)peer->id,
					       "dir", dir, "request_id", id,
					       "native", peer->native,
					       "image", jsonrpc_conn_image(peer->conn)));
	if (!text || remote_send_fd(project->fd, text, peer->fd)) {
		remote_log(server, "client %lu: cannot give the client to the project "
			   "server %ld: %s", peer->id, (long)project->pid,
			   strerror(errno));
		remote_reply(peer, id, "cannot reach the project server", fy_invalid);
		return;
	}
	remote_log(server, "client %lu: directory '%s' of the project server %ld",
		   peer->id, dir, (long)project->pid);
	/* Keep a copy of the socket until the project server has received it. */
	limbo = calloc(1, sizeof(*limbo));
	if (limbo) {
		limbo->client = peer->id;
		limbo->fd = fcntl(peer->fd, F_DUPFD_CLOEXEC, 3);
		if (limbo->fd < 0) {
			free(limbo);
		} else {
			limbo->next = project->limbo;
			project->limbo = limbo;
		}
	}
	peer->role = REMOTE_PEER_HANDED;
	peer->failed = true;
}

/* The directory @path that a client works in: in this project, or another. */
static void remote_project(struct fyai_remote_peer *peer, fy_generic id,
			   const char *path)
{
	struct fyai_remote_server *server = peer->server;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	const char *why = NULL;
	char *dir, *root;

	if (remote_resolve(server, path, &dir, &root, &why)) {
		remote_log(server, "client %lu: refused the directory '%s': %s",
			   peer->id, path, why);
		remote_reply(peer, id, why, fy_invalid);
		return;
	}
	if (server->base || strcmp(root, server->root)) {
		/* `--once` ends with its sessions, and a project server that it
		 * started would end with it. */
		if (server->once) {
			remote_log(server, "client %lu: refused the directory '%s' of "
				   "the project '%s'", peer->id, dir, root);
			remote_reply(peer, id, "the directory is in another project, "
				     "which a server that serves once does not serve",
				     fy_invalid);
		} else {
			remote_hand_over(peer, id, dir, root);
		}
	} else {
		free(peer->dir);
		peer->dir = dir;
		dir = NULL;
		remote_log(server, "client %lu: directory '%s'", peer->id, peer->dir);
		remote_reply(peer, id, NULL, fy_mapping(gb, "root", server->root,
							"dir", peer->dir));
	}
	free(dir);
	free(root);
}

static size_t remote_watchers(const struct fyai_remote_session *session)
{
	const struct fyai_remote_peer *watcher;
	size_t n = 0;

	for (watcher = session->watchers; watcher; watcher = watcher->wnext)
		n++;
	return n;
}

/*
 * Replace the record of the live sessions, which a client that has no
 * connection reads: the resume picker. It names this process and its
 * sessions and is not canonical state.
 */
static void remote_record(struct fyai_remote_server *server)
{
	struct fy_generic_builder *gb;
	struct fyai_remote_session *session;
	fy_generic sessions = fy_seq_empty;
	const char *text;
	char *path = NULL, *tmp = NULL;
	size_t len, off = 0;
	ssize_t n;
	int fd = -1;

	server->dirty = false;
	if (!server->record)
		return;
	gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	for (session = server->sessions; session; session = session->next)
		if (!session->exited && session->branch)
			sessions = fy_append(gb, sessions, fy_mapping(gb,
				"branch", session->branch,
				"pid", (long long)session->pid,
				"busy", (_Bool)(session->due > 0),
				"waiting", session->waiting,
				"attached", (_Bool)(session->peer || session->driven)));
	text = emit_json_string(gb, fy_mapping(gb, "pid", (long long)getpid(),
					       "sessions", sessions));
	path = strdup(server->record);
	if (!text || !path || asprintf(&tmp, "%s.%ld", path, (long)getpid()) < 0) {
		tmp = NULL;
		goto err;
	}
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		goto err;
	for (len = strlen(text); off < len; off += (size_t)n) {
		n = write(fd, text + off, len - off);
		if (n < 0 && errno == EINTR) {
			n = 0;
			continue;
		}
		if (n < 0)
			goto err;
	}
	if (close(fd) < 0) {
		fd = -1;
		goto err;
	}
	fd = -1;
	if (rename(tmp, path))
		goto err;
	goto out;
err:
	remote_log(server, "cannot write the record of the sessions: %s",
		   strerror(errno));
	if (tmp)
		unlink(tmp);
out:
	if (fd >= 0)
		close(fd);
	free(tmp);
	free(path);
}

/* `serve --once`: the sessions have run, and none runs a turn or has a client. */
static bool remote_once_done(const struct fyai_remote_server *server)
{
	const struct fyai_remote_session *session;

	if (!server->once || !server->started)
		return false;
	for (session = server->sessions; session; session = session->next)
		if (!session->exited && (session->due || session->peer ||
					 session->driven || session->watchers ||
					 session->keep))
			return false;
	return true;
}

/* The state of the server, for `serve status`. */
static fy_generic remote_status(struct fyai_remote_server *server)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	struct fyai_remote_project *project;
	struct fyai_remote_session *session;
	fy_generic projects = fy_seq_empty, sessions = fy_seq_empty, value;

	for (project = server->projects; project; project = project->next)
		if (!project->exited)
			projects = fy_append(gb, projects, fy_mapping(gb,
				"root", project->root, "pid", (long long)project->pid));
	for (session = server->sessions; session; session = session->next)
		if (!session->exited)
			sessions = fy_append(gb, sessions, fy_mapping(gb,
				"pid", (long long)session->pid,
				"branch", session->branch ? session->branch : "",
				"attached", (_Bool)(session->peer || session->driven),
				"local", session->driven,
				"busy", (_Bool)(session->due > 0),
				"waiting", session->waiting,
				"watchers", (long long)remote_watchers(session)));
	value = fy_mapping(gb, "pid", (long long)getpid());
	if (server->path)
		value = fy_assoc(gb, value, "socket", server->path);
	if (server->base)
		return fy_assoc(gb, fy_assoc(gb, value, "projects_dir", server->base),
				"projects", projects);
	value = fy_assoc(gb, fy_assoc(gb, value, "project", server->root),
			 "sessions", sessions);
	return fy_empty(projects) ? value : fy_assoc(gb, value, "projects", projects);
}

/*
 * fyai_hello: a client of the fyai protocol of this version makes its session
 * a fyai session. It gets the version of this program and the features that
 * both have.
 */
static void remote_hello_answer(struct fyai_remote_peer *peer, fy_generic id,
				fy_generic request)
{
	struct fyai_remote_server *server = peer->server;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	long long protocol = fy_get(request, "protocol", -1LL);
	const char *version = fy_get(request, "version", "unknown");
	fy_generic theirs = fy_get(request, "features", fy_seq_empty), common;
	const char *feature;
	size_t i;

	if (protocol != FYAI_REMOTE_PROTOCOL) {
		remote_log(server, "client %lu: refused: fyai %s speaks the protocol "
			   "%lld, this server %d", peer->id, version, protocol,
			   FYAI_REMOTE_PROTOCOL);
		remote_reply(peer, id, fy_sprintfa("the client speaks the fyai protocol "
			     "%lld and the server (fyai %s) speaks %d: use the same "
			     "version of fyai on both", protocol, VERSION,
			     FYAI_REMOTE_PROTOCOL), fy_invalid);
		return;
	}
	common = fy_seq_empty;
	fy_foreach(feature, theirs)
		for (i = 0; i < ARRAY_SIZE(remote_features); i++)
			if (feature && !strcmp(feature, remote_features[i]))
				common = fy_append(gb, common,
						   fy_value(gb, remote_features[i]));
	/* Images of the same byte order and word size are spoken by both. */
	fy_foreach(feature, theirs)
		if (feature && jsonrpc_image_feature() && !peer->lines_only &&
		    !strcmp(feature, jsonrpc_image_feature()))
			common = fy_append(gb, common,
					   fy_value(gb, jsonrpc_image_feature()));
	peer->native = true;
	remote_log(server, "client %lu: fyai %s, protocol %lld", peer->id, version,
		   protocol);
	remote_reply(peer, id, NULL, fy_mapping(gb, "version", VERSION,
						"protocol", FYAI_REMOTE_PROTOCOL,
						"features", common, "session", "fyai"));
	/* The reply is the last line: the client sends images after it. */
	fy_foreach(feature, common)
		if (jsonrpc_image_feature() && !peer->lines_only &&
		    !strcmp(feature, jsonrpc_image_feature()))
			(void)jsonrpc_conn_set_image(peer->conn);
}

/* A request of the fyai protocol from a client that did not say hello. */
static bool remote_needs_hello(struct fyai_remote_peer *peer, fy_generic id,
			       const char *subtype)
{
	if (peer->native)
		return false;
	remote_log(peer->server, "client %lu: refused '%s': not a fyai session",
		   peer->id, subtype);
	if (fy_is_string(id))
		remote_reply(peer, id, fy_sprintfa("'%s' needs a fyai session: send "
			     "fyai_hello first", subtype), fy_invalid);
	else
		peer->failed = true;
	return true;
}

/* A request about the server itself: its state, or to stop. */
static void remote_server_request(struct fyai_remote_peer *peer, fy_generic id,
				  const char *subtype)
{
	struct fyai_remote_server *server = peer->server;

	if (!strcmp(subtype, "fyai_server_stop")) {
		remote_log(server, "client %lu: stop requested", peer->id);
		server->stop = true;
	}
	remote_reply(peer, id, NULL, remote_status(server));
}

/*
 * The branch that `resume` selects for @peer with @args, {session, last, all},
 * as it selects here: the session it names, or with --last the session updated
 * last that started in the directory of the client. NULL with the reason in
 * *@why. The caller frees the name.
 */
static char *remote_resume_branch(struct fyai_remote_peer *peer, fy_generic args,
				  const char **why)
{
	const char *session = fy_get(args, "session", (const char *)NULL);
	bool all = fy_get(args, "all", false);
	char *name;

	if (session) {
		name = strdup(session);
		*why = "cannot keep the name of the session";
		return name;
	}
	if (!fy_get(args, "last", false)) {
		*why = "resume at a server: name the session, or use --last";
		return NULL;
	}
	name = fyai_peek_branch_pick(NULL, NULL, peer->dir ? peer->dir :
				     peer->server->root, all, NULL);
	*why = all ? "resume: no resumable session; run fyai to start one" :
	       "resume: no resumable session started in this directory; use "
	       "--all, or run fyai to start one";
	if (name)
		remote_log(peer->server, "client %lu: resume selects '%s'", peer->id,
			   name);
	return name;
}

/* True when @words is a sequence of strings: a command line. */
static bool remote_words_valid(fy_generic words)
{
	fy_generic word;

	if (!fy_is_sequence(words))
		return false;
	fy_foreach(word, words)
		if (!fy_is_string(word))
			return false;
	return true;
}

/*
 * fyai_session_adopt: a worker of an interactive session, which runs on its
 * own, gives its channel to the server: this connection becomes the channel
 * of a session on @branch, and the worker answers the clients that attach.
 * With `driven`, the worker registers: its own client drives it, and the
 * connection carries its frames to the watchers until the client leaves it to
 * the server (remote_worker_driven()). The answer goes out on the connection
 * first.
 */
static void remote_adopt(struct fyai_remote_peer *peer, fy_generic id,
			 fy_generic request)
{
	struct fyai_remote_server *server = peer->server;
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	struct fyai_remote_session *session;
	const char *branch = fy_get(request, "branch", (const char *)NULL);
	long long pid = fy_get(request, "pid", 0LL);

	if (!branch || !fyai_branch_name_ref_valid(branch) || pid <= 0 ||
	    peer->session || server->base) {
		remote_reply(peer, id, "the worker names no branch or process",
			     fy_invalid);
		return;
	}
	for (session = server->sessions; session; session = session->next)
		if (!session->exited && session->branch &&
		    !strcmp(session->branch, branch)) {
			remote_log(server, "client %lu: refused: a worker runs "
				   "'%s'", peer->id, branch);
			remote_reply(peer, id, "a worker runs the branch",
				     fy_invalid);
			return;
		}
	session = calloc(1, sizeof(*session));
	if (!session) {
		remote_reply(peer, id, "cannot keep the session", fy_invalid);
		return;
	}
	session->branch = fyai_ctx_keep_str(ctx, branch);
	if (!session->branch) {
		free(session);
		remote_reply(peer, id, "cannot keep the branch", fy_invalid);
		return;
	}
	session->server = server;
	session->pid = (pid_t)pid;
	session->adopted = true;
	session->driven = fy_get(request, "driven", false);
	session->keep = !session->driven && fy_get(request, "keep", false);
	session->due = fy_get(request, "busy", false) ? 1 : 0;
	/* The connection moves to the session; the peer is dropped at the
	 * next reap. The record is written before the answer goes out, so a
	 * client that the worker's end starts next finds the session in it. */
	session->worker = peer->conn;
	session->fd = peer->fd;
	jsonrpc_conn_expect_close(session->worker);
	(void)jsonrpc_conn_frames(session->worker, remote_worker_frame, session);
	session->next = server->sessions;
	server->sessions = session;
	server->started = true;
	remote_record(server);
	remote_reply(peer, id, NULL, fy_map_empty);
	peer->conn = NULL;
	peer->fd = -1;
	peer->role = REMOTE_PEER_HANDED;
	peer->failed = true;
	remote_log(server, "client %lu: %s the worker %ld on branch '%s'",
		   peer->id, session->driven ? "registered" : "adopted", (long)pid,
		   branch);
}

/*
 * A request of a supervisor (doc/fyai-fleet-sdd.md, section 7) to the live
 * session on a branch, also one that a client drives: fyai_session_send {text,
 * from}, fyai_session_interrupt, fyai_session_answer {answer}. It goes to the
 * worker under the prefix of a watcher, so the answer comes back to @peer,
 * which then watches the session quietly: it gets its answers and no frame.
 */
static void remote_supervise(struct fyai_remote_peer *peer, fy_generic id,
			     const char *subtype, fy_generic request)
{
	struct fyai_remote_server *server = peer->server;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	struct fyai_remote_session *session;
	const char *branch = fy_get(request, "branch", (const char *)NULL);
	fy_generic order;

	for (session = server->sessions; branch && session; session = session->next)
		if (!session->exited && session->branch &&
		    !strcmp(session->branch, branch))
			break;
	if (!session || !session->worker) {
		remote_reply(peer, id, "no live session on the branch", fy_invalid);
		return;
	}
	if (peer->session) {
		remote_reply(peer, id, "the client has a session", fy_invalid);
		return;
	}
	if (!strcmp(subtype, "fyai_session_send"))
		order = fy_mapping(gb, "subtype", "fyai_message",
				   "text", fy_get(request, "text", ""),
				   "from", fy_get(request, "from", ""));
	else if (!strcmp(subtype, "fyai_session_answer"))
		order = fy_mapping(gb, "subtype", "fyai_supervise_answer",
				   "answer", fy_get(request, "answer", ""));
	else if (!strcmp(subtype, "fyai_session_lineage"))
		order = fy_mapping(gb, "subtype", "fyai_fleet_lineage",
				   "parent", fy_get(request, "parent", ""),
				   "task", fy_get(request, "task", 0LL));
	else
		order = fy_mapping(gb, "subtype", "interrupt");
	peer->role = REMOTE_PEER_WATCHER;
	peer->quiet = true;
	peer->session = session;
	peer->wnext = session->watchers;
	session->watchers = peer;
	remote_log(server, "client %lu: %s to the worker %ld on branch '%s'",
		   peer->id, subtype + strlen("fyai_session_"), (long)session->pid,
		   branch);
	if (fyai_remote_send(session->worker, fy_mapping(gb,
			"type", "control_request",
			"request_id", fy_stringf(gb, REMOTE_WATCH_PREFIX "%lu:%s",
						 peer->id, fy_castp(&id, "")),
			"request", order)))
		peer->failed = true;
}

/*
 * fyai_session_close: end the live session on a branch. Its worker gets
 * SIGTERM (SIGKILL with @force), its clients fail, and the registry drops it.
 * What the branch stored stays; a turn in flight is lost.
 */
static void remote_session_close(struct fyai_remote_peer *peer, fy_generic id,
				 fy_generic request)
{
	struct fyai_remote_server *server = peer->server;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	struct fyai_remote_session *session;
	struct fyai_remote_peer *watcher;
	const char *branch = fy_get(request, "branch", (const char *)NULL);
	bool force = fy_get(request, "force", false), busy;

	for (session = server->sessions; branch && session; session = session->next)
		if (!session->exited && session->branch &&
		    !strcmp(session->branch, branch))
			break;
	if (!session) {
		remote_reply(peer, id, "no live session on the branch", fy_invalid);
		return;
	}
	if (session->peer == peer || peer->session == session) {
		remote_reply(peer, id, "a client does not close its own session",
			     fy_invalid);
		return;
	}
	busy = session->due > 0;
	remote_log(server, "client %lu: closes the session of '%s'%s", peer->id,
		   branch, force ? " (force)" : "");
	remote_reply(peer, id, NULL, fy_mapping(gb, "branch", branch,
						"busy", busy));
	(void)kill(session->pid, force ? SIGKILL : SIGTERM);
	/* A child of the server: its exit watcher drops the session. */
	if (!session->adopted)
		return;
	/* No child to watch: the channel ends it. */
	jsonrpc_conn_destroy(session->worker);
	session->worker = NULL;
	if (session->fd >= 0)
		close(session->fd);
	session->fd = -1;
	session->exited = true;
	session->keep = false;
	server->dirty = true;
	if (session->peer)
		session->peer->failed = true;
	for (watcher = session->watchers; watcher; watcher = watcher->wnext)
		watcher->failed = true;
}

static void remote_admit_frame(struct jsonrpc_conn *conn, fy_generic frame,
			       void *data)
{
	struct fyai_remote_peer *peer = data;
	struct fyai_remote_server *server = peer->server;
	fy_generic request, id, words;
	const char *subtype, *text, *why = NULL;
	bool watch, supervise;
	char *resumed;

	(void)conn;
	if (peer->failed)
		return;
	if (peer->role == REMOTE_PEER_PENDING) {
		remote_authenticate(peer, frame);
		return;
	}
	request = fy_get(frame, "request", fy_invalid);
	id = fy_get(frame, "request_id", fy_invalid);
	subtype = fy_equal(fy_get(frame, "type", ""), "control_request") ?
		  fy_get(request, "subtype", "") : "";
	if (!strcmp(subtype, "fyai_hello")) {
		if (!fy_is_string(id) || peer->native) {
			peer->failed = true;
			return;
		}
		remote_hello_answer(peer, id, request);
		return;
	}
	if (!strcmp(subtype, "fyai_server_stop") ||
	    !strcmp(subtype, "fyai_server_status")) {
		if (!fy_is_string(id)) {
			peer->failed = true;
			return;
		}
		if (!remote_needs_hello(peer, id, subtype))
			remote_server_request(peer, id, subtype);
		return;
	}
	if (!strcmp(subtype, "fyai_project")) {
		text = fy_get(request, "path", (const char *)NULL);
		if (!fy_is_string(id) || !text || !*text || peer->session) {
			peer->failed = true;
			return;
		}
		if (!remote_needs_hello(peer, id, subtype))
			remote_project(peer, id, text);
		return;
	}
	/* A server of projects serves no session itself. */
	if (server->base) {
		remote_log(server, "client %lu: refused: no project named", peer->id);
		if (fy_is_string(id))
			remote_reply(peer, id, "the server serves projects: name one",
				     fy_invalid);
		peer->failed = true;
		return;
	}
	if (peer->role == REMOTE_PEER_WATCHER) {
		remote_watch_request(peer, frame, id, subtype);
		return;
	}
	if (peer->run) {
		remote_run_client(peer, frame);
		return;
	}
	words = fy_get(request, "args", fy_seq_empty);
	if (subtype[0] && (!fy_is_string(id) || !remote_words_valid(words))) {
		peer->failed = true;
		return;
	}
	supervise = fy_get(request, "supervise", false);
	watch = fy_get(request, "watch", false);
	/* An SDK session attaches to a branch; the rest is the fyai protocol. */
	if ((!strcmp(subtype, "fyai_run") ||
	     (!strcmp(subtype, "fyai_session_attach") &&
	      (watch || !fy_empty(words) ||
	       fy_is_valid(fy_get(request, "resume", fy_invalid)) ||
	       !fy_get(request, "branch", (const char *)NULL)))) &&
	    remote_needs_hello(peer, id, subtype))
		return;
	if (!strcmp(subtype, "fyai_session_close")) {
		if (!remote_needs_hello(peer, id, subtype))
			remote_session_close(peer, id, request);
		return;
	}
	if (!strcmp(subtype, "fyai_session_send") ||
	    !strcmp(subtype, "fyai_session_interrupt") ||
	    !strcmp(subtype, "fyai_session_lineage") ||
	    !strcmp(subtype, "fyai_session_answer")) {
		if (!remote_needs_hello(peer, id, subtype))
			remote_supervise(peer, id, subtype, request);
		return;
	}
	if (!strcmp(subtype, "fyai_session_adopt")) {
		if (!remote_needs_hello(peer, id, subtype))
			remote_adopt(peer, id, request);
		return;
	}
	if (!strcmp(subtype, "fyai_session_attach")) {
		text = fy_get(request, "branch", (const char *)NULL);
		resumed = NULL;
		if (!text && fy_is_mapping(fy_get(request, "resume", fy_invalid))) {
			resumed = remote_resume_branch(peer, fy_get(request, "resume",
								    fy_invalid), &why);
			if (!resumed) {
				remote_reply(peer, id, why, fy_invalid);
				return;
			}
			text = resumed;
		}
		if (text && !fyai_branch_name_ref_valid(text)) {
			free(resumed);
			peer->failed = true;
			return;
		}
		if (watch && !text) {
			remote_reply(peer, id, "a watcher names the branch to watch",
				     fy_invalid);
		} else if (text) {
			remote_attach(peer, id, text, words, watch || supervise,
				      supervise);
			if (peer->session && !watch && fy_get(request, "keep", false)) {
				peer->session->keep = true;
				remote_log(server, "client %lu: the session of '%s' is "
					   "kept", peer->id, text);
			}
			free(resumed);
		} else if (peer->session) {
			remote_reply(peer, id, "the client has a session", fy_invalid);
		} else if (remote_spawn(peer, NULL, words)) {
			remote_reply(peer, id, "cannot start a session worker", fy_invalid);
		} else {
			remote_reply(peer, id, NULL, fy_map_empty);
		}
		return;
	}
	if (!strcmp(subtype, "fyai_run")) {
		if (peer->session) {
			remote_reply(peer, id, "the client has a session", fy_invalid);
			return;
		}
		if (remote_run_start(peer, words, fy_get(request, "stdin", false),
				     fy_get(request, "cols", 0L)))
			remote_reply(peer, id, "cannot start the command", fy_invalid);
		else
			remote_reply(peer, id, NULL, fy_mapping(fyai_ctx_transient_gb(
				fyai_cmd_call_ctx(server->call)), "pid", (long long)peer->run->pid));
		return;
	}
	if (!peer->session && remote_spawn(peer, NULL, fy_seq_empty)) {
		peer->failed = true;
		return;
	}
	/* The prefix of a watcher is not for a client that drives. */
	if (!strncmp(fy_get(frame, "request_id", ""), REMOTE_WATCH_PREFIX,
		     strlen(REMOTE_WATCH_PREFIX))) {
		peer->failed = true;
		return;
	}
	/* A message that is not queued behind a turn starts a turn, which
	 * ends with a result. */
	if (fy_equal(fy_get(frame, "type", ""), "user") &&
	    !fy_get(frame, "queued", false)) {
		peer->session->due++;
		server->dirty = true;
	}
	if (fyai_remote_send(peer->session->worker, frame))
		peer->failed = true;
}

static enum fyai_event_action remote_auth_timeout(const struct fyai_event *ev)
{
	struct fyai_remote_peer *peer = ev->userdata;

	remote_log(peer->server, "client %lu: refused: no launch token in time",
		   peer->id);
	peer->failed = true;
	return FYAIEA_CONTINUE;
}

/* Keep the connection @fd of a client, named @id in the log. */
static struct fyai_remote_peer *remote_peer_add(struct fyai_remote_server *server, int fd,
					   unsigned long id, bool authenticated)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	struct fyai_remote_peer *peer;
	int rc;

	peer = calloc(1, sizeof(*peer));
	if (!peer) {
		close(fd);
		fyai_warning(ctx, "cannot keep a client");
		return NULL;
	}
	peer->id = id;
	peer->role = authenticated ? REMOTE_PEER_CLIENT : REMOTE_PEER_PENDING;
	peer->server = server;
	peer->fd = fd;
	peer->next = server->peers;
	server->peers = peer;
	peer->conn = jsonrpc_conn_socket(ctx, fd, 0, "remote client", NULL);
	if (!peer->conn) {
		peer->failed = true;
		return peer;
	}
	jsonrpc_conn_expect_close(peer->conn);
	rc = jsonrpc_conn_frames(peer->conn, remote_admit_frame, peer);
	if (!rc && !authenticated)
		rc = fyai_event_add_timer(fyai_ctx_loop(ctx), REMOTE_ADMIT_MS,
					  0, remote_auth_timeout, peer,
					  &peer->deadline);
	peer->failed = rc != 0;
	return peer;
}

static enum fyai_event_action remote_accept(const struct fyai_event *ev)
{
	struct fyai_remote_server *server = ev->userdata;
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	uid_t uid;
	int fd, rc;

	fd = accept(server->fd, NULL, NULL);
	if (fd >= 0 && fyai_remote_cloexec(fd)) {
		close(fd);
		fd = -1;
	}
	if (fd < 0) {
		if (errno != EAGAIN && errno != EINTR && !server->accept_failed) {
			server->accept_failed = true;
			fyai_warning(ctx, "cannot accept a client: %s",
				     strerror(errno));
		}
		return FYAIEA_CONTINUE;
	}
	rc = fyai_host_peer_uid(fd, &uid);
	if (rc || uid != geteuid()) {
		remote_log(server, "refused a client of another user");
		close(fd);
		return FYAIEA_CONTINUE;
	}
	server->clients++;
	remote_log(server, "client %lu: connected", server->clients);
	remote_peer_add(server, fd, server->clients, !server->require_token);
	return FYAIEA_CONTINUE;
}

/* The HTTP front gives a client that passed its token: a socket of lines. */
static int remote_http_admit(void *data, int fd,
			     const struct fyai_remote_identity *id, const char *from,
			     bool tls)
{
	struct fyai_remote_server *server = data;
	struct fyai_remote_peer *peer;

	server->clients++;
	remote_log(server, "client %lu: connected over %s from %s%s%s",
		   server->clients, tls ? "https" : "http", from,
		   id->subject[0] ? " as " : "", id->subject);
	peer = remote_peer_add(server, fd, server->clients, true);
	if (!peer)
		return -1;
	/* The WebSocket carries a frame as a message: lines, never images. */
	peer->lines_only = true;
	return 0;
}

static void remote_http_log(void *data, const char *line)
{
	remote_log(data, "%s", line);
}

/*
 * The bearer token of the HTTP clients: the 64 hexadecimal digits in @path,
 * which only the user may read, else a new random one that the file then
 * holds. The default is ~/.fyai/serve.token.
 */
static int remote_http_token(struct fyai_remote_server *server, const char *file,
			     char **pathp)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	unsigned char random[32];
	char *path = NULL, *dir, line[66];
	const char *home = getenv("HOME");
	struct stat st;
	ssize_t n = 0;
	int fd = -1, i, rc = -1;

	line[0] = '\0';
	if (file) {
		path = strdup(file);
	} else {
		fyai_error_check(ctx, home && *home, out, "no HOME for the token file");
		if (asprintf(&dir, "%s/.fyai", home) < 0)
			dir = NULL;
		fyai_error_check(ctx, dir, out, "cannot make the path of the token file");
		i = fyai_remote_mkdirs(ctx->cfg, strlen(home), dir);
		free(dir);
		if (i)
			goto out;
		if (asprintf(&path, "%s/.fyai/serve.token", home) < 0)
			path = NULL;
	}
	fyai_error_check(ctx, path, out, "cannot make the path of the token file");
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		i = fstat(fd, &st);
		fyai_error_check(ctx, !i && S_ISREG(st.st_mode) && !(st.st_mode & 077),
				 out, "the token file '%s' must be a file that only "
				 "you can read", path);
		do {
			n = read(fd, line, sizeof(line) - 1);
		} while (n < 0 && errno == EINTR);
		fyai_error_check(ctx, n >= 64, out, "the token file '%s' holds no "
				 "token of 64 digits", path);
		for (i = 0; i < 64; i++)
			if (!isxdigit((unsigned char)line[i]))
				n = 0;
		fyai_error_check(ctx, n >= 64 && (n == 64 || line[64] == '\n'), out,
				 "the token file '%s' holds no token of 64 digits", path);
		memcpy(server->http_token, line, 64);
	} else {
		fyai_error_check(ctx, file == NULL && errno == ENOENT, out,
				 "cannot open the token file '%s': %s", path,
				 strerror(errno));
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		fyai_error_check(ctx, fd >= 0, out, "cannot make the token file "
				 "'%s': %s", path, strerror(errno));
		fyai_host_random(random, sizeof(random));
		for (i = 0; i < 32; i++)
			snprintf(server->http_token + i * 2, 3, "%02x", random[i]);
		memset(random, 0, sizeof(random));
		memcpy(line, server->http_token, 64);
		line[64] = '\n';
		n = write(fd, line, 65);
		fyai_error_check(ctx, n == 65, out, "cannot write the token file '%s'",
				 path);
	}
	*pathp = path;
	path = NULL;
	rc = 0;
out:
	memset(line, 0, sizeof(line));
	if (fd >= 0)
		close(fd);
	free(path);
	return rc;
}

/* True for a loopback address in the form fyai_remote_http_open() reports. */
static bool remote_http_loopback(const char *bound)
{
	return !strncmp(bound, "127.", 4) || !strncmp(bound, "[::1]:", 6);
}

/*
 * Open the listeners of @server: each entry of @listens is ADDRESS, or
 * http://ADDRESS or https://ADDRESS. A bare address is plain http.
 */
static int remote_http_start(struct fyai_remote_server *server, fy_generic listens,
			     const char *token_file, const char *tls_cert,
			     const char *tls_key, fy_generic tls_names)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	struct fyai_remote_auth auth = { .verify = fyai_remote_token_verify,
					 .data = server->http_token };
	struct fyai_remote_http_ops ops = { .admit = remote_http_admit,
		.log = remote_http_log, .data = server };
	char bound[96], fp[FYAI_TLS_FP_SIZE], *path = NULL;
	const char **names = NULL, *spec, *address, *name;
	SSL_CTX *tls = NULL;
	size_t n = 0;
	bool secure;
	int rc = -1;

	if (fy_is_valid(tls_names))
		n = fy_len(tls_names);
	names = calloc(n + 1, sizeof(*names));
	fyai_error_check(ctx, names, out, "cannot keep the TLS names");
	n = 0;
	if (fy_is_valid(tls_names))
		fy_foreach(name, tls_names)
			names[n++] = name;
	if (remote_http_token(server, token_file, &path))
		goto out;
	server->http = fyai_remote_http_new(ctx, &auth, &ops);
	if (!server->http)
		goto out;
	fy_foreach(spec, listens) {
		secure = !strncmp(spec, "https://", 8);
		address = secure ? spec + 8 : !strncmp(spec, "http://", 7) ? spec + 7 : spec;
		if (secure && !tls) {
			tls = fyai_tls_server_ctx(ctx, tls_cert, tls_key, names, fp);
			if (!tls)
				goto out;
			remote_log(server, "the key of the server is %s", fp);
		}
		if (fyai_remote_http_listen(server->http, address, secure ? tls : NULL,
					    bound, sizeof(bound)))
			goto out;
		remote_log(server, "listening on %s://%s/ with the token in '%s'",
			   secure ? "https" : "http", bound, path);
		if (!secure && !remote_http_loopback(bound))
			remote_log(server, "warning: plain http on %s: the token and "
				   "the frames cross the network in the clear", bound);
	}
	rc = 0;
out:
	SSL_CTX_free(tls);
	free(names);
	free(path);
	return rc;
}

/*
 * In a project server behind a front: a client that the front gives, with
 * the directory it asked for and the request that the answer goes to. The
 * end of the channel is the end of the front, and of this server.
 */
static enum fyai_event_action remote_front_receive(const struct fyai_event *ev)
{
	struct fyai_remote_server *server = ev->userdata;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_cmd_call_ctx(server->call));
	struct fyai_remote_peer *peer;
	char buf[PATH_MAX + 512];
	const char *dir;
	fy_generic msg, id;
	uint64_t client;
	ssize_t n;
	int fd;

	n = remote_recv_fd(server->front_fd, buf, sizeof(buf), &fd);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return FYAIEA_CONTINUE;
	if (n <= 0) {
		if (n < 0)
			remote_log(server, "the channel of the server of projects "
				   "failed: %s", strerror(errno));
		else
			remote_log(server, "the server of projects closed: stopping");
		server->stop = true;
		return FYAIEA_CONTINUE;
	}
	msg = parse_json_string(gb, buf);
	/* The front keeps its copy of the client until it hears this. */
	client = fy_get(msg, "client", 0UL);
	(void)send(server->front_fd, &client, sizeof(client), MSG_NOSIGNAL);
	id = fy_get(msg, "request_id", fy_invalid);
	dir = fy_get(msg, "dir", (const char *)NULL);
	if (!fy_is_string(id) || !dir || !remote_path_within(server->root, dir)) {
		remote_log(server, "the server of projects gave a client without "
			   "a directory of '%s'", server->root);
		close(fd);
		return FYAIEA_CONTINUE;
	}
	peer = remote_peer_add(server, fd, fy_get(msg, "client", 0UL), true);
	if (!peer || peer->failed)
		return FYAIEA_CONTINUE;
	peer->native = fy_get(msg, "native", false);
	/* The client switched to images when it said hello to the front. */
	if (fy_get(msg, "image", false))
		(void)jsonrpc_conn_set_image(peer->conn);
	peer->dir = strdup(dir);
	if (!peer->dir) {
		remote_reply(peer, id, "cannot keep the directory", fy_invalid);
		peer->failed = true;
		return FYAIEA_CONTINUE;
	}
	remote_log(server, "client %lu: directory '%s'", peer->id, peer->dir);
	remote_reply(peer, id, NULL, fy_mapping(gb, "root", server->root,
						"dir", peer->dir));
	return FYAIEA_CONTINUE;
}

static void remote_session_free(struct fyai_remote_session *session)
{
	struct fyai_event_loop *loop = fyai_ctx_loop(fyai_cmd_call_ctx(session->server->call));

	jsonrpc_conn_destroy(session->worker);
	if (session->fd >= 0)
		close(session->fd);
	fyai_event_source_remove(session->child);
	if (!session->exited && !session->adopted)
		fyai_event_child_terminate(loop, session->pid, 0, 1000, NULL);
	free(session);
}

/*
 * Drop the peers that failed or closed. A session that no branch names has
 * nothing to reattach to: its worker ends with its client, and its input
 * closes. A worker or a project server that exited leaves the registry.
 */
static void remote_reap(struct fyai_remote_server *server)
{
	struct fyai_remote_peer **link = &server->peers, *peer;
	struct fyai_remote_session **slink = &server->sessions, *session;
	struct fyai_remote_project **plink = &server->projects, *project;
	struct fyai_remote_peer **wlink, *watcher;

	while ((peer = *link)) {
		remote_run_settle(peer);
		/* The last frames go out before the peer goes. */
		if (peer->drain && !peer->failed && !jsonrpc_conn_closed(peer->conn) &&
		    jsonrpc_conn_has_output(peer->conn)) {
			link = &peer->next;
			continue;
		}
		if (peer->drain)
			peer->failed = true;
		if (!peer->failed && !jsonrpc_conn_closed(peer->conn)) {
			link = &peer->next;
			continue;
		}
		*link = peer->next;
		if (peer->role != REMOTE_PEER_HANDED)
			remote_log(server, "client %lu: disconnected", peer->id);
		session = peer->session;
		if (session && peer->role == REMOTE_PEER_WATCHER) {
			for (wlink = &session->watchers; *wlink != peer;
			     wlink = &(*wlink)->wnext)
				;
			*wlink = peer->wnext;
			if (session->snapshot_peer == peer)
				session->snapshot_peer = NULL;
		} else if (session) {
			session->peer = NULL;
			server->dirty = true;
			if (session->snapshot_peer == peer)
				session->snapshot_peer = NULL;
			if (session->anonymous && !session->exited) {
				for (watcher = session->watchers; watcher;
				     watcher = watcher->wnext)
					watcher->failed = true;
				jsonrpc_conn_destroy(session->worker);
				session->worker = NULL;
				close(session->fd);
				session->fd = -1;
			}
		}
		remote_run_free(peer->run);
		fyai_event_source_remove(peer->deadline);
		jsonrpc_conn_destroy(peer->conn);
		if (peer->fd >= 0)
			close(peer->fd);
		free(peer->dir);
		free(peer);
	}
	/* An adopted worker has no child to watch: its channel is its life. */
	for (session = server->sessions; session; session = session->next)
		if (session->adopted && !session->exited &&
		    jsonrpc_conn_closed(session->worker)) {
			session->exited = true;
			server->dirty = true;
			remote_session_ended(session);
			remote_log(server, "worker %ld (branch '%s') closed its "
				   "channel", (long)session->pid,
				   session->branch ? session->branch : "none");
		}
	while ((session = *slink)) {
		if (!session->exited || session->peer || session->watchers) {
			slink = &session->next;
			continue;
		}
		*slink = session->next;
		remote_session_free(session);
	}
	while ((project = *plink)) {
		if (!project->exited) {
			plink = &project->next;
			continue;
		}
		*plink = project->next;
		remote_project_free(project);
	}
}

/*
 * Bind @fd to @address. A socket there that nobody serves is what a server
 * that died left: it is removed. Any other file, and a socket that answers,
 * stays, and the bind fails.
 */
static int remote_bind(struct fyai_remote_server *server, int fd,
		       const struct sockaddr_un *address)
{
	struct stat st;
	mode_t mask;
	int probe, rc;

	mask = umask(0077);
	rc = bind(fd, (const struct sockaddr *)address, sizeof(*address));
	if (rc && errno == EADDRINUSE && !lstat(address->sun_path, &st) &&
	    S_ISSOCK(st.st_mode)) {
		probe = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fyai_remote_cloexec(probe)) {
			close(probe);
			probe = -1;
		}
		if (probe >= 0 &&
		    connect(probe, (const struct sockaddr *)address,
			    sizeof(*address)) && errno == ECONNREFUSED &&
		    !unlink(address->sun_path)) {
			remote_log(server, "removed the stale endpoint '%s'",
				   address->sun_path);
			rc = bind(fd, (const struct sockaddr *)address,
				  sizeof(*address));
		} else {
			errno = EADDRINUSE;
		}
		if (probe >= 0)
			close(probe);
	}
	umask(mask);
	return rc;
}

/* Bind and listen on the endpoint of @server, and watch it. */
static int remote_listen(struct fyai_remote_server *server)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	int rc;

	fyai_error_check(ctx, strlen(server->path) < sizeof(address.sun_path), err,
			 "the socket path '%s' is too long", server->path);
	memcpy(address.sun_path, server->path, strlen(server->path) + 1);
	server->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	rc = fyai_remote_cloexec(server->fd);
	if (!rc && server->fd >= 0)
		rc = fcntl(server->fd, F_SETFL,
			   fcntl(server->fd, F_GETFL) | O_NONBLOCK) < 0;
	fyai_error_check(ctx, server->fd >= 0 && !rc, err, "socket: %s",
			 strerror(errno));
	rc = remote_bind(server, server->fd, &address);
	if (rc) {
		fyai_error(ctx, "bind '%s': %s", server->path, strerror(errno));
		free(server->path);
		server->path = NULL;
		goto err;
	}
	rc = chmod(server->path, 0600);
	if (!rc)
		rc = listen(server->fd, 16);
	fyai_error_check(ctx, !rc, err, "listen '%s': %s", server->path,
			 strerror(errno));
	rc = fyai_event_add_fd(fyai_ctx_loop(ctx), server->fd, FYAIEV_READ,
			       remote_accept, server, &server->listener);
	fyai_error_check(ctx, !rc, err, "cannot watch the endpoint");
	return 0;
err:
	return -1;
}

/* Write the launch token to @token_fd, which closes. */
static int remote_token(struct fyai_remote_server *server, int token_fd)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	unsigned char random[32];
	char line[66];
	ssize_t count;
	int i;

	server->require_token = true;
	fyai_host_random(random, sizeof(random));
	for (i = 0; i < 32; i++)
		snprintf(server->token + i * 2, 3, "%02x", random[i]);
	memset(random, 0, sizeof(random));
	memcpy(line, server->token, 64);
	line[64] = '\n';
	do {
		count = write(token_fd, line, 65);
	} while (count < 0 && errno == EINTR);
	close(token_fd);
	memset(line, 0, sizeof(line));
	fyai_error_check(ctx, count == 65, err, "cannot deliver the launch token");
	return 0;
err:
	return -1;
}

/* `serve --daemon`: tell the process that started this one that it serves. */
static void remote_ready(struct fyai_remote_server *server, int fd)
{
	char *line = NULL;
	ssize_t n;
	int len;

	if (fd < 0)
		return;
	len = asprintf(&line, "%ld\t%s\n", (long)getpid(), server->path);
	if (len > 0) {
		do {
			n = write(fd, line, (size_t)len);
		} while (n < 0 && errno == EINTR);
		if (n != len)
			remote_log(server, "cannot report to the process that "
				   "started the server: %s", strerror(errno));
	}
	free(line);
	close(fd);
}

/* The descriptor of `serve --daemon` that the server reports on, or -1. */
static int remote_ready_fd(void)
{
	const char *text = getenv(REMOTE_READY_ENV);
	char *end;
	long fd;
	int rc;

	if (!text)
		return -1;
	fd = strtol(text, &end, 10);
	rc = *end;
	unsetenv(REMOTE_READY_ENV);
	if (rc || fd < 3 || fd > INT_MAX || fcntl((int)fd, F_SETFD, FD_CLOEXEC))
		return -1;
	return (int)fd;
}

/* The default log of `serve --daemon`: .fyai/logs/serve.log of @top. */
static char *remote_log_path(struct fyai_cfg *cfg, bool projects)
{
	const char *home = getenv("HOME");
	char *top, *dir = NULL, *path = NULL;

	if (projects) {
		top = home && *home ? strdup(home) : NULL;
	} else {
		top = fyai_discover_project_root();
		if (!top)
			top = getcwd(NULL, 0);
	}
	fyai_cfg_error_check(cfg, top, out, "cannot find the directory of the log");
	if (asprintf(&dir, "%s/.fyai/logs", top) < 0) {
		dir = NULL;
		goto out;
	}
	if (fyai_remote_mkdirs(cfg, strlen(top), dir))
		goto out;
	if (asprintf(&path, "%s/serve.log", dir) < 0)
		path = NULL;
out:
	free(dir);
	free(top);
	return path;
}

int fyai_cmd_remote_serve_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	const char *log = fy_get(args, "log", (const char *)NULL);
	int ready[2] = { -1, -1 }, logfd = -1, null = -1, rc = -1;
	char *path = NULL, line[PATH_MAX + 64], fdbuf[16];
	size_t len = 0;
	ssize_t n;
	pid_t pid;

	if (!fy_get(args, "daemon", false))
		return 0;
	fyai_cfg_error_check(cfg, fy_get(args, "front_fd", -1LL) < 0, out,
			     "a project server of a server of projects is not a daemon");
	path = log ? strdup(log) :
	       remote_log_path(cfg, fy_is_string(fy_get(args, "projects", fy_invalid)));
	fyai_cfg_error_check(cfg, path, out, "cannot make the path of the log");
	logfd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	fyai_cfg_error_check(cfg, logfd >= 0, out, "open '%s': %s", path,
			     strerror(errno));
	null = open("/dev/null", O_RDWR | O_CLOEXEC);
	fyai_cfg_error_check(cfg, null >= 0, out, "open '/dev/null': %s",
			     strerror(errno));
	rc = pipe(ready);
	if (!rc)
		rc = fyai_remote_cloexec(ready[0]);
	fyai_cfg_error_check(cfg, !rc, out, "cannot make the channel of the "
			     "daemon: %s", strerror(errno));
	rc = -1;
	pid = fork();
	fyai_cfg_error_check(cfg, pid >= 0, out, "cannot fork the daemon: %s",
			     strerror(errno));
	if (!pid) {
		/* The server: a session of its own, the log as its output. */
		if (setsid() < 0 || dup2(null, STDIN_FILENO) < 0 ||
		    dup2(logfd, STDOUT_FILENO) < 0 || dup2(logfd, STDERR_FILENO) < 0)
			_exit(1);
		close(ready[0]);
		snprintf(fdbuf, sizeof(fdbuf), "%d", ready[1]);
		setenv(REMOTE_READY_ENV, fdbuf, 1);
		ready[0] = ready[1] = -1;
		rc = 0;
		goto out;
	}
	close(ready[1]);
	ready[1] = -1;
	/* The server writes its process and its endpoint, by a tab, when it serves. */
	while (len < sizeof(line) - 1) {
		do {
			n = read(ready[0], line + len, sizeof(line) - 1 - len);
		} while (n < 0 && errno == EINTR);
		if (n <= 0)
			break;
		len += (size_t)n;
		if (memchr(line, '\n', len))
			break;
	}
	line[len] = '\0';
	fyai_cfg_error_check(cfg, len && line[len - 1] == '\n' && strchr(line, '\t'),
			     out, "the server did not start; the log '%s' says why",
			     path);
	line[len - 1] = '\0';
	cfg->serve_daemon = fy_gb_intern_string(cfg->gb, fy_sprintfa("%s\t%s",
							 line, path));
	fyai_cfg_error_check(cfg, cfg->serve_daemon, out,
			     "cannot keep the report of the daemon");
	rc = 0;
out:
	if (ready[0] >= 0)
		close(ready[0]);
	if (ready[1] >= 0)
		close(ready[1]);
	if (logfd >= 0)
		close(logfd);
	if (null >= 0)
		close(null);
	free(path);
	return rc;
}

/* The parent of `serve --daemon`: the server that started, as the result. */
static int remote_daemon_result(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *report = fyai_cmd_call_ctx(call)->cfg->serve_daemon;
	const char *socket = strchr(report, '\t');
	const char *log = socket ? strchr(socket + 1, '\t') : NULL;

	fyai_error_check(fyai_cmd_call_ctx(call), socket && log, err,
			 "cannot read the report of the daemon");
	*result = fy_mapping(call->gb, "pid", strtoll(report, NULL, 10),
			     "socket", fy_stringf(call->gb, "%.*s",
						  (int)(log - socket - 1), socket + 1),
			     "log", log + 1);
	return 0;
err:
	return -1;
}

/*
 * A project server behind a front keeps the record of its sessions where its
 * own endpoint would keep it, so a list of the project and its picker find
 * them; a client joins them through the front. A project with a server of its
 * own keeps that server's record.
 */
static void remote_front_record(struct fyai_remote_server *server)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(server->call);
	struct stat st;
	char *socket;

	socket = fyai_remote_socket_path(ctx, NULL, false, true);
	if (!socket) {
		fyai_diag_reset(fyai_ctx_diag(ctx));
		remote_log(server, "cannot make the directory of the record");
		return;
	}
	if (stat(socket, &st) || !S_ISSOCK(st.st_mode))
		server->record = fyai_remote_record_path(socket);
	free(socket);
	remote_record(server);
}

int fyai_cmd_remote_serve(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_remote_server server = { .call = call, .fd = -1, .front_fd = -1 };
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	struct fyai_event_loop *loop = fyai_ctx_loop(ctx);
	const char *projects = fyai_cmd_arg_str(call, "projects");
	fy_generic listens = fy_get(call->args, "listen", fy_invalid);
	bool listening = fy_is_valid(listens) && !fy_empty(listens);
	struct fyai_remote_project *project;
	struct fyai_remote_session *session;
	struct fyai_remote_peer *peer;
	int rc = -1, token_fd, ready_fd = -1;
	fyai_event_ms_t deadline;
	bool success = false;

	if (ctx->cfg->serve_daemon)
		return remote_daemon_result(call, result);
	ready_fd = remote_ready_fd();
	token_fd = fy_get(call->args, "token_fd", -1);
	server.front_fd = fy_get(call->args, "front_fd", -1);
	server.once = fy_get(call->args, "once", false);
	fyai_error_check(ctx, !server.once || (!projects && server.front_fd < 0),
			 out, "--once serves one project, not projects");
	if (projects) {
		server.base = realpath(projects, NULL);
		fyai_error_check(ctx, server.base, out, "the directory of the "
				 "projects '%s': %s", projects, strerror(errno));
	} else {
		server.root = fyai_discover_project_root();
		if (!server.root)
			server.root = getcwd(NULL, 0);
		fyai_error_check(ctx, server.root, out, "cannot find the project "
				 "directory");
	}
	if (server.front_fd >= 0) {
		fyai_error_check(ctx, !projects && token_fd < 0 && !listening,
				 out, "a project server of a server of projects has no "
				 "endpoint");
		rc = fyai_remote_cloexec(server.front_fd);
		if (!rc)
			rc = fcntl(server.front_fd, F_SETFL,
				   fcntl(server.front_fd, F_GETFL) | O_NONBLOCK) < 0;
		fyai_error_check(ctx, !rc, out, "the channel of the server of "
				 "projects: %s", strerror(errno));
		rc = fyai_event_add_fd(loop, server.front_fd, FYAIEV_READ,
				       remote_front_receive, &server, &server.front);
		fyai_error_check(ctx, !rc, out, "cannot watch the server of projects");
		remote_log(&server, "serving the project '%s' for the server of "
			   "projects", server.root);
		remote_front_record(&server);
	} else {
		server.path = fyai_remote_socket_path(ctx, fyai_cmd_arg_str(call, "socket"),
						 projects, true);
		/* The Unix socket is the last to open: a client that waits for it
		 * finds the listeners ready, and their log lines written. */
		if (!server.path)
			goto out;
		if (listening) {
			rc = remote_http_start(&server, listens,
					       fyai_cmd_arg_str(call, "token_file"),
					       fyai_cmd_arg_str(call, "tls_cert"),
					       fyai_cmd_arg_str(call, "tls_key"),
					       fy_get(call->args, "tls_name", fy_invalid));
			if (rc)
				goto out;
		}
		if (remote_listen(&server))
			goto out;
		if (token_fd >= 0) {
			rc = remote_token(&server, token_fd);
			token_fd = -1;
			if (rc)
				goto out;
		}
		remote_log(&server, "serving %s '%s' at '%s'",
			   projects ? "the projects under" : "the project",
			   projects ? server.base : server.root, server.path);
		remote_ready(&server, ready_fd);
		ready_fd = -1;
		/* The record says that the server listens: a session that waits
		 * for a server sees it appear (doc/fyai-fleet-sdd.md, 6.2). */
		server.record = fyai_remote_record_path(server.path);
		remote_record(&server);
	}
	rc = 0;
	while (!ctx->loop->terminate_pending && !fyai_interrupt_check(ctx) &&
	       !server.stop) {
		fyai_event_interrupt_ack(ctx->loop);
		rc = fyai_event_loop_step(loop, -1);
		if (rc < 0)
			break;
		remote_reap(&server);
		if (server.dirty)
			remote_record(&server);
		if (remote_once_done(&server)) {
			remote_log(&server, "the sessions ended their turns");
			break;
		}
	}
	fyai_error_check(ctx, rc >= 0, out, "the event loop of the endpoint "
			 "stopped");
	/* The answer to a stop request goes out before its connection closes. */
	deadline = fyai_event_now_ms() + 1000;
	for (;;) {
		for (peer = server.peers; peer; peer = peer->next)
			if (!peer->failed && jsonrpc_conn_has_output(peer->conn))
				break;
		if (!peer || fyai_event_now_ms() >= deadline ||
		    fyai_event_loop_step(loop, 100) < 0)
			break;
	}
	remote_log(&server, "stopped");
	success = true;
out:
	fyai_remote_http_close(server.http);
	fyai_event_source_remove(server.listener);
	fyai_event_source_remove(server.front);
	fyai_event_interrupt_ack(ctx->loop);
	while (server.peers) {
		server.peers->failed = true;
		remote_reap(&server);
	}
	while ((session = server.sessions)) {
		server.sessions = session->next;
		remote_session_free(session);
	}
	while ((project = server.projects)) {
		server.projects = project->next;
		remote_project_free(project);
	}
	if (server.fd >= 0)
		close(server.fd);
	if (server.path && server.fd >= 0)
		unlink(server.path);
	if (server.record)
		unlink(server.record);
	free(server.record);
	if (server.front_fd >= 0)
		close(server.front_fd);
	if (token_fd >= 0)
		close(token_fd);
	if (ready_fd >= 0)
		close(ready_fd);
	memset(server.token, 0, sizeof(server.token));
	memset(server.http_token, 0, sizeof(server.http_token));
	if (server.frame_gb)
		fy_generic_builder_destroy(server.frame_gb);
	free(server.path);
	free(server.base);
	free(server.root);
	return success ? 0 : -1;
}
