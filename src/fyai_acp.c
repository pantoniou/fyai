/*
 * fyai_acp.c - Agent Client Protocol over a client-owned stdio connection
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "fyai_host.h"
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/rand.h>

#include "fyai.h"
#include "fyai_acp.h"
#include "fyai_branch.h"
#include "fyai_cmd.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_output.h"
#include "fyai_storage.h"
#include "fyai_transport_boot.h"
#include "fyai_turn.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

struct acp_server;

static void acp_close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

struct acp_session {
	struct acp_session *next;
	struct acp_server *server;
	char *id;
	char *cwd;
	char *message_id;
	char *agent_id;
	fy_generic prompt_id;
	struct jsonrpc_conn *worker;
	struct jsonrpc_request *run;
	pid_t pid;
	int worker_in;
	int worker_out;
	bool open;
	bool busy;
	bool inserted;
	bool cancelling;
	bool update_failed;
};

struct acp_server {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *client;
	struct acp_session *sessions;
	bool initialized;
	int version;
};

struct acp_worker {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	fy_generic run_id;
	char *text;
	char *message_id;
	char *agent_id;
	fy_generic content;
	bool pending;
	bool ran;
	bool cancelled;
	bool done;
};

static fy_generic acp_error(struct fyai_ctx *ctx, long long code,
			    const char *message)
{
	return fy_mapping(fyai_ctx_transient_gb(ctx),
			  "code", code, "message", message);
}

static bool acp_random_id(char *out, size_t size, const char *prefix)
{
	unsigned char bytes[16];
	size_t i, base;

	base = strlen(prefix);
	if (size < base + sizeof(bytes) * 2 + 1 ||
	    RAND_bytes(bytes, sizeof(bytes)) != 1)
		return false;
	memcpy(out, prefix, base);
	for (i = 0; i < sizeof(bytes); i++)
		snprintf(out + base + i * 2, 3, "%02x", bytes[i]);
	return true;
}

static int acp_update_frame(struct acp_server *server, const char *id,
			    fy_generic update)
{
	return jsonrpc_notify(server->client, "session/update",
		fy_mapping(fyai_ctx_transient_gb(server->ctx),
			   "sessionId", id, "update", update));
}

static int acp_update(struct acp_server *server, const char *id,
		      fy_generic update)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(server->ctx);
	fy_generic kind, block;
	const char *message_id;

	if (server->version != 1)
		return acp_update_frame(server, id, update);
	kind = fy_get(update, "sessionUpdate", fy_invalid);
	if (fy_any_equal(kind, "state_update", "agent_message"))
		return 0;
	if (fy_equal(kind, "user_message")) {
		message_id = fy_get(update, "messageId", "");
		fy_foreach(block, fy_get(update, "content", fy_seq_empty))
			if (acp_update_frame(server, id, fy_mapping(gb,
				"sessionUpdate", "user_message_chunk",
				"messageId", message_id, "content", block)))
				return -1;
		return 0;
	}
	if (fy_equal(kind, "tool_call_update"))
		update = fy_assoc(gb, update, "sessionUpdate", "tool_call");
	return acp_update_frame(server, id, update);
}

static struct acp_session *acp_session_find(struct acp_server *server,
					   const char *id)
{
	struct acp_session *s;

	for (s = server->sessions; s; s = s->next)
		if (!strcmp(s->id, id))
			return s;
	return NULL;
}

static struct acp_session *acp_session_add(struct acp_server *server,
					  const char *id, const char *cwd)
{
	struct acp_session *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	s->id = strdup(id);
	s->cwd = strdup(cwd);
	if (!s->id || !s->cwd) {
		free(s->id);
		free(s->cwd);
		free(s);
		return NULL;
	}
	s->server = server;
	s->worker_in = -1;
	s->worker_out = -1;
	s->prompt_id = fy_invalid;
	s->open = true;
	s->next = server->sessions;
	server->sessions = s;
	return s;
}

static bool acp_cwd_valid(const char *cwd)
{
	struct stat st;

	return cwd && cwd[0] == '/' && !stat(cwd, &st) && S_ISDIR(st.st_mode);
}

static bool acp_mcp_empty(fy_generic params)
{
	fy_generic servers = fy_get(params, "mcpServers", fy_invalid);

	return fy_is_invalid(servers) || fy_is_null(servers) ||
		(fy_is_sequence(servers) && fy_empty(servers));
}

static char *acp_prompt_text(fy_generic blocks)
{
	struct response_buffer out = { 0 };
	fy_generic block, type;
	const char *s, *uri, *name;
	char *text;

	if (!fy_is_sequence(blocks) || fy_empty(blocks))
		return NULL;
	fy_foreach(block, blocks) {
		if (!fy_is_mapping(block))
			goto invalid;
		type = fy_get(block, "type", fy_invalid);
		if (fy_equal(type, "text")) {
			s = fy_get(block, "text", (const char *)NULL);
			if (!s)
				goto invalid;
		} else if (fy_equal(type, "resource_link")) {
			uri = fy_get(block, "uri", (const char *)NULL);
			name = fy_get(block, "name", (const char *)NULL);
			if (!uri || !*uri || !name || !*name)
				goto invalid;
			text = fy_sprintfa("%s (%s)", name, uri);
			s = text;
		} else {
			goto invalid;
		}
		if (out.len && response_buffer_append_data(&out, "\n", 1))
			goto invalid;
		if (response_buffer_append_data(&out, s, strlen(s)))
			goto invalid;
	}
	if (!out.data)
		return strdup("");
	return out.data;
invalid:
	free(out.data);
	return NULL;
}

static void acp_worker_release(struct acp_session *s)
{
	int status;

	if (s->run) {
		jsonrpc_request_destroy(s->run);
		s->run = NULL;
	}
	if (s->worker) {
		jsonrpc_conn_expect_close(s->worker);
		jsonrpc_conn_destroy(s->worker);
		s->worker = NULL;
	}
	acp_close_fd(&s->worker_in);
	acp_close_fd(&s->worker_out);
	if (s->pid > 0) {
		if (waitpid(s->pid, &status, WNOHANG) == 0)
			(void)kill(s->pid, SIGTERM);
		(void)waitpid(s->pid, &status, 0);
		s->pid = 0;
	}
	free(s->message_id);
	free(s->agent_id);
	s->message_id = s->agent_id = NULL;
	s->prompt_id = fy_invalid;
	s->inserted = s->cancelling = s->update_failed = false;
}

static void acp_run_complete(struct jsonrpc_request *req, void *userdata)
{
	struct acp_session *s = userdata;
	struct acp_server *server = s->server;
	struct fyai_ctx *ctx = server->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	bool ok = jsonrpc_request_ok(req);
	const char *reason;

	if (!s->inserted && fy_is_valid(s->prompt_id)) {
		if (jsonrpc_conn_respond(server->client, s->prompt_id,
			fy_invalid, acp_error(ctx, -32000,
				"the prompt could not be inserted")))
			fyai_error(ctx, "could not report ACP prompt failure");
	}
	reason = s->update_failed ? "error" :
		s->cancelling ? "cancelled" : ok ? "end_turn" : "error";
	if (s->inserted && server->version == 1) {
		if (jsonrpc_conn_respond(server->client, s->prompt_id,
			fy_mapping(gb,
				   "stopReason", s->update_failed ? "refusal" :
					s->cancelling ? "cancelled" : ok ? "end_turn" :
					"refusal"), fy_invalid))
			fyai_error(ctx, "could not report ACP turn result");
	}
	if (s->inserted && server->version == 2) {
		if (acp_update(server, s->id,
			fy_mapping(gb,
				   "sessionUpdate", "state_update",
				   "state", "idle", "stopReason", reason)))
			fyai_error(ctx, "could not report ACP turn completion");
	}
	s->busy = false;
}

static fy_generic acp_child_serve(struct jsonrpc_conn *conn,
				  const char *method, fy_generic params,
				  fy_generic id, void *userdata,
				  fy_generic *errorp)
{
	struct acp_session *s = userdata;
	struct acp_server *server = s->server;
	fy_generic update;

	(void)conn;
	(void)id;
	(void)errorp;
	if (!strcmp(method, "acp/inserted")) {
		if (s->busy && !s->inserted &&
		    fy_equal(fy_get(params, "messageId"), s->message_id)) {
			if (server->version == 2 &&
			    jsonrpc_conn_respond(server->client, s->prompt_id,
					fy_mapping(fyai_ctx_transient_gb(server->ctx),
						   "messageId", s->message_id),
					fy_invalid)) {
				fyai_error(server->ctx,
					   "could not acknowledge ACP prompt '%s'",
					   s->message_id);
				s->update_failed = true;
				return fy_invalid;
			}
			s->inserted = true;
		}
	} else if (!strcmp(method, "acp/update")) {
		update = fy_get(params, "update", fy_invalid);
		if (s->busy && fy_is_mapping(update) &&
		    acp_update(server, s->id, update)) {
			s->update_failed = true;
			fyai_error(server->ctx,
				   "could not forward ACP update for '%s'", s->id);
		}
	}
	return fy_invalid;
}

static int acp_worker_transport_attach(int sync_fd, int agent_fd, int ctl_fd)
{
	uint64_t exec_id;
	char value[32];
	const char *old;
	int old_fd;
	ssize_t len;

	len = read(sync_fd, &exec_id, sizeof(exec_id));
	if (len != sizeof(exec_id))
		return -1;
	old = getenv(FYAI_TRANSPORT_FD_ENV);
	old_fd = old ? atoi(old) : -1;
	if (old_fd > STDERR_FILENO)
		acp_close_fd(&old_fd);
	old = getenv(FYAI_TRANSPORT_CTL_ENV);
	old_fd = old ? atoi(old) : -1;
	if (old_fd > STDERR_FILENO)
		acp_close_fd(&old_fd);
	snprintf(value, sizeof(value), "%d", agent_fd);
	if (setenv(FYAI_TRANSPORT_FD_ENV, value, 1))
		return -1;
	snprintf(value, sizeof(value), "%d", ctl_fd);
	if (setenv(FYAI_TRANSPORT_CTL_ENV, value, 1))
		return -1;
	snprintf(value, sizeof(value), "%llu", (unsigned long long)exec_id);
	return setenv(FYAI_TRANSPORT_EXEC_ENV, value, 1);
}

static int acp_worker_transport_admit(struct fyai_ctx *ctx, pid_t pid,
				      int agent_fd, int ctl_fd, int sync_fd)
{
	uint64_t exec_id;
	int pidfd = -1, rc;
	ssize_t len;

	pidfd = fyai_host_pidfd_open(pid);
	rc = fyai_transport_admit_child(ctx, pid, pidfd, agent_fd, ctl_fd,
					&exec_id);
	acp_close_fd(&pidfd);
	if (rc)
		return -1;
	len = write(sync_fd, &exec_id, sizeof(exec_id));
	fyai_error_check(ctx, len == sizeof(exec_id), err,
			 "could not start ACP worker transport: %s", strerror(errno));
	return 0;
err:
	return -1;
}

static int acp_spawn_worker(struct acp_session *s, fy_generic content,
			    const char *text)
{
	struct acp_server *server = s->server;
	struct fyai_ctx *ctx = server->ctx;
	char *const *orig = ctx->cfg->argv;
	char **args = NULL;
	char *self = NULL;
	int input[2] = { -1, -1 }, output[2] = { -1, -1 };
	int agent[2] = { -1, -1 }, ctl[2] = { -1, -1 };
	int sync[2] = { -1, -1 };
	int i, n, rc = -1, admit_rc;
	pid_t pid;
	fy_generic params;

	for (n = 0; orig[n] && strcmp(orig[n], "acp"); n++)
		;
	fyai_error_check(ctx, orig[n], out,
			 "ACP worker command is missing");
	self = realpath(orig[0], NULL);
	fyai_error_check(ctx, self || !strchr(orig[0], '/'), out,
			 "could not resolve ACP executable '%s': %s",
			 orig[0], strerror(errno));
	args = calloc((size_t)n + 5, sizeof(*args));
	fyai_error_check(ctx, args, out,
			 "could not allocate ACP worker arguments");
	for (i = 0; i < n; i++)
		args[i] = orig[i];
	args[n++] = "--branch";
	args[n++] = s->id;
	args[n++] = "acp";
	args[n++] = "--worker";
	args[n] = NULL;
	fyai_error_check(ctx, !pipe(input) && !pipe(output), out,
			 "could not create ACP worker pipes: %s", strerror(errno));
	if (ctx->tclient) {
		admit_rc = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, agent);
		if (!admit_rc)
			admit_rc = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctl);
		if (!admit_rc)
			admit_rc = pipe(sync);
		fyai_error_check(ctx, !admit_rc, out,
				 "could not create ACP transport channels: %s",
				 strerror(errno));
	}
	pid = fork();
	fyai_error_check(ctx, pid >= 0, out,
			 "could not fork ACP worker: %s", strerror(errno));
	if (!pid) {
		acp_close_fd(&input[1]);
		acp_close_fd(&output[0]);
		if (ctx->tclient) {
			acp_close_fd(&agent[1]);
			acp_close_fd(&ctl[1]);
			acp_close_fd(&sync[1]);
			if (acp_worker_transport_attach(sync[0], agent[0], ctl[0]))
				_exit(126);
			acp_close_fd(&sync[0]);
		}
		if (chdir(s->cwd) || dup2(input[0], STDIN_FILENO) < 0 ||
		    dup2(output[1], STDOUT_FILENO) < 0)
			_exit(126);
		acp_close_fd(&input[0]);
		acp_close_fd(&output[1]);
		if (self)
			execv(self, args);
		else
			execvp(orig[0], args);
		_exit(127);
	}
	acp_close_fd(&input[0]);
	acp_close_fd(&output[1]);
	s->pid = pid;
	if (ctx->tclient) {
		acp_close_fd(&agent[0]);
		acp_close_fd(&ctl[0]);
		acp_close_fd(&sync[0]);
		admit_rc = acp_worker_transport_admit(ctx, pid, agent[1], ctl[1],
						    sync[1]);
		fyai_error_check(ctx, !admit_rc, out,
				 "could not admit ACP worker to transport");
		acp_close_fd(&agent[1]);
		acp_close_fd(&ctl[1]);
		acp_close_fd(&sync[1]);
	}
	s->worker_in = input[1];
	s->worker_out = output[0];
	input[1] = output[0] = -1;
	s->worker = jsonrpc_conn_stdio(ctx, s->worker_in, s->worker_out,
					0, "acp worker", NULL);
	fyai_error_check(ctx, s->worker, out,
			 "could not open ACP worker connection");
	fyai_error_check(ctx,
			 !jsonrpc_conn_serve(s->worker, acp_child_serve, s), out,
			 "could not serve ACP worker connection");
	params = fy_mapping(fyai_ctx_transient_gb(ctx),
		"text", text, "content", content,
		"messageId", s->message_id, "agentId", s->agent_id);
	s->run = jsonrpc_request_submit(s->worker, "acp/run", params,
		jsonrpc_conn_next_id(s->worker), false, acp_run_complete, s);
	fyai_error_check(ctx, s->run, out,
			 "could not submit ACP worker turn");
	rc = 0;
out:
	acp_close_fd(&input[0]);
	acp_close_fd(&input[1]);
	acp_close_fd(&output[0]);
	acp_close_fd(&output[1]);
	for (i = 0; i < 2; i++) {
		acp_close_fd(&agent[i]);
		acp_close_fd(&ctl[i]);
		acp_close_fd(&sync[i]);
	}
	free(args);
	free(self);
	if (rc)
		acp_worker_release(s);
	return rc;
}

static int acp_replay(struct acp_server *server, struct acp_session *s,
		      fy_generic head)
{
	struct fyai_ctx *ctx = server->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct fyai_turn_stack stack;
	fy_generic messages, m, role, id, content, blocks;
	char fallback[64], agent_id[96];
	char *user_id = NULL;
	const char *value;
	size_t i, seq = 0;
	int rc = -1;

	if (fyai_turn_stack_init(&stack, head, fy_invalid))
		return -1;
	for (i = 0; i < stack.count; i++) {
		messages = fy_get(stack.items[i], "messages", fy_seq_empty);
		fy_foreach(m, messages) {
			role = fy_get(m, "role", fy_invalid);
			content = fy_get(m, "content", fy_invalid);
			if (!fy_is_string(content))
				continue;
			value = fy_castp(&content, "");
			if (fy_equal(role, "user")) {
				id = fy_get(m, "acp_message_id", fy_invalid);
				free(user_id);
				if (!fy_is_string(id)) {
					snprintf(fallback, sizeof(fallback), "msg_replay_%zu", ++seq);
					user_id = strdup(fallback);
				} else {
					user_id = strdup(fy_castp(&id, ""));
				}
				if (!user_id)
					goto out;
				blocks = fy_get(m, "acp_content", fy_invalid);
				if (!fy_is_sequence(blocks))
					blocks = fy_sequence(gb,
						fy_mapping(gb, "type", "text",
							"text", value));
				if (acp_update(server, s->id,
					fy_mapping(gb,
					   "sessionUpdate", "user_message",
					   "messageId", user_id,
					   "content", blocks)))
					goto out;
			} else if (fy_equal(role, "assistant") && user_id) {
				snprintf(agent_id, sizeof(agent_id), "%s-a", user_id);
				if (acp_update(server, s->id,
					fy_mapping(gb,
					   "sessionUpdate", "agent_message",
					   "messageId", agent_id,
					   "content", fy_seq_empty)))
					goto out;
				if (acp_update(server, s->id,
					fy_mapping(gb,
					   "sessionUpdate", "agent_message_chunk",
					   "messageId", agent_id,
					   "content", fy_mapping(gb,
						"type", "text", "text", value))))
					goto out;
			}
		}
	}
	rc = 0;
out:
	free(user_id);
	fyai_turn_stack_cleanup(&stack);
	return rc;
}

static fy_generic acp_initialize(struct acp_server *server,
				 struct jsonrpc_conn *conn, fy_generic params,
				 fy_generic *errorp)
{
	struct fyai_ctx *ctx = server->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);

	if (server->initialized ||
	    !fy_generic_is_int(fy_get(params, "protocolVersion", fy_invalid))) {
		*errorp = acp_error(ctx, -32602, "invalid ACP protocol version");
		return fy_invalid;
	}
	server->version = fy_get(params, "protocolVersion", 0LL) == 1 ? 1 : 2;
	server->initialized = true;
	if (server->version == 2)
		jsonrpc_conn_accept_batches(conn);
	if (server->version == 1)
		return fy_mapping(gb,
			"protocolVersion", 1LL,
			"agentCapabilities", fy_mapping(gb,
				"loadSession", true,
				"sessionCapabilities", fy_mapping(gb,
					"list", fy_map_empty,
					"resume", fy_map_empty)),
			"agentInfo", fy_mapping(gb,
				"name", "fyai", "title", "fyai", "version", VERSION),
			"authMethods", fy_seq_empty);
	return fy_mapping(gb,
		"protocolVersion", 2LL,
		"capabilities", fy_mapping(gb, "session", fy_map_empty),
		"info", fy_mapping(gb,
			"name", "fyai", "title", "fyai", "version", VERSION));
}

static fy_generic acp_session_new(struct acp_server *server,
				  fy_generic params, fy_generic *errorp)
{
	struct fyai_ctx *ctx = server->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const char *cwd = fy_get(params, "cwd", (const char *)NULL);
	char newid[64];

	if (!acp_cwd_valid(cwd) || !acp_mcp_empty(params)) {
		*errorp = acp_error(ctx, -32602,
			"absolute cwd and an empty mcpServers list are required");
		return fy_invalid;
	}
	if (!acp_random_id(newid, sizeof(newid), "acp/") ||
	    fyai_branch_import(ctx, newid, fy_null,
		fy_mapping(gb, "kind", "acp"), fy_value(cwd), NULL) ||
	    !acp_session_add(server, newid, cwd)) {
		*errorp = acp_error(ctx, -32000, "cannot create ACP session");
		return fy_invalid;
	}
	return fy_mapping(gb, "sessionId", newid);
}

static fy_generic acp_session_list(struct acp_server *server,
				   fy_generic params, fy_generic *errorp)
{
	struct fyai_ctx *ctx = server->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct fyai_branch b;
	fy_generic name, entry, sessions, cursor;
	const char *cwd = fy_get(params, "cwd", (const char *)NULL);
	const char *nm;

	cursor = fy_get(params, "cursor", fy_invalid);
	if ((cwd && cwd[0] != '/') ||
	    (fy_is_valid(cursor) && !fy_is_null(cursor))) {
		*errorp = acp_error(ctx, -32602, "invalid session/list filter");
		return fy_invalid;
	}
	sessions = fy_seq_empty;
	fy_foreach_key_value(name, entry, ctx->arena_branches) {
		nm = fy_castp(&name, "");
		if (strncmp(nm, "acp/", 4) ||
		    !fyai_branch_decode(entry, &b) || !fy_is_string(b.cwd))
			continue;
		if (cwd && strcmp(cwd, fy_castp(&b.cwd, "")))
			continue;
		sessions = fy_append(gb, sessions,
			fy_mapping(gb, "sessionId", nm,
				"cwd", fy_castp(&b.cwd, "")));
	}
	return fy_mapping(gb, "sessions", sessions);
}

static fy_generic acp_session_resume(struct acp_server *server,
				     struct acp_session *s,
				     const struct fyai_branch *branch,
				     const char *sid, const char *method,
				     fy_generic params, fy_generic *errorp)
{
	struct fyai_ctx *ctx = server->ctx;
	fy_generic cursor;
	const char *cwd = fy_get(params, "cwd", (const char *)NULL);

	if (!acp_cwd_valid(cwd) || !fyai_branch_cwd(branch) ||
	    strcmp(cwd, fyai_branch_cwd(branch)) || !acp_mcp_empty(params)) {
		*errorp = acp_error(ctx, -32602,
			"session cwd or MCP servers are invalid");
		return fy_invalid;
	}
	if (s && s->busy) {
		*errorp = acp_error(ctx, -32003, "session is busy");
		return fy_invalid;
	}
	if (!s)
		s = acp_session_add(server, sid, cwd);
	if (!s) {
		*errorp = acp_error(ctx, -32000, "cannot open session");
		return fy_invalid;
	}
	s->open = true;
	cursor = fy_get(params, "replayFrom", fy_invalid);
	if (server->version == 1 && !strcmp(method, "session/load")) {
		if (acp_replay(server, s, branch->head)) {
			*errorp = acp_error(ctx, -32000, "cannot replay session");
			return fy_invalid;
		}
	} else if (server->version == 2 &&
		   fy_is_valid(cursor) && !fy_is_null(cursor)) {
		if (!fy_equal(fy_get(cursor, "type"), "start") ||
		    acp_replay(server, s, branch->head)) {
			*errorp = acp_error(ctx, -32602,
				"unsupported replay cursor");
			return fy_invalid;
		}
	}
	return fy_map_empty;
}

static fy_generic acp_session_prompt(struct acp_server *server,
				     struct jsonrpc_conn *conn,
				     struct acp_session *s,
				     fy_generic params, fy_generic id,
				     fy_generic *errorp)
{
	struct fyai_ctx *ctx = server->ctx;
	fy_generic blocks;
	char msgid[64], agentid[96];
	char *text;
	int rc;

	if (!fy_is_valid(id) || s->busy) {
		*errorp = acp_error(ctx, -32003, "session is busy");
		return fy_invalid;
	}
	blocks = fy_get(params, "prompt", fy_invalid);
	text = acp_prompt_text(blocks);
	if (!text || !acp_random_id(msgid, sizeof(msgid), "msg_") ||
	    snprintf(agentid, sizeof(agentid), "%s-a", msgid) >=
		(int)sizeof(agentid)) {
		free(text);
		*errorp = acp_error(ctx, -32602, "unsupported prompt content");
		return fy_invalid;
	}
	s->message_id = strdup(msgid);
	s->agent_id = strdup(agentid);
	s->prompt_id = fy_gb_internalize(ctx->cfg->gb, id);
	s->busy = true;
	rc = !s->message_id || !s->agent_id ||
	     !fy_is_valid(s->prompt_id) ? -1 :
	     acp_spawn_worker(s, blocks, text);
	free(text);
	if (rc) {
		s->busy = false;
		acp_worker_release(s);
		*errorp = acp_error(ctx, -32000, "cannot start ACP turn");
		return fy_invalid;
	}
	jsonrpc_conn_defer(conn);
	return fy_invalid;
}

static fy_generic acp_session_stop(struct acp_server *server,
				   struct acp_session *s, bool close,
				   fy_generic *errorp)
{
	if (s->busy) {
		s->cancelling = true;
		if (jsonrpc_notify(s->worker, "acp/cancel", fy_map_empty)) {
			*errorp = acp_error(server->ctx, -32000,
				"cannot cancel ACP turn");
			return fy_invalid;
		}
	}
	if (close)
		s->open = false;
	return close ? fy_map_empty : fy_invalid;
}

static fy_generic acp_serve(struct jsonrpc_conn *conn, const char *method,
                            fy_generic params, fy_generic id, void *userdata,
                            fy_generic *errorp)
{
	struct acp_server *server = userdata;
	struct fyai_ctx *ctx = server->ctx;
	struct fyai_branch branch;
	struct acp_session *s;
	const char *sid;

	if (!strcmp(method, "initialize"))
		return acp_initialize(server, conn, params, errorp);
	if (!server->initialized) {
		*errorp = acp_error(ctx, -32002, "initialize first");
		return fy_invalid;
	}
	if (jsonrpc_conn_in_batch(conn) && strcmp(method, "session/list")) {
		*errorp = acp_error(ctx, -32600, "lifecycle request in batch");
		return fy_invalid;
	}
	if (fyai_branches_refresh(ctx)) {
		*errorp = acp_error(ctx, -32000, "cannot refresh sessions");
		return fy_invalid;
	}
	if (!strcmp(method, "session/new"))
		return acp_session_new(server, params, errorp);
	if (!strcmp(method, "session/list"))
		return acp_session_list(server, params, errorp);
	if ((server->version != 1 && !strcmp(method, "session/load")) ||
	    (strcmp(method, "session/resume") && strcmp(method, "session/load") &&
	     strcmp(method, "session/close") && strcmp(method, "session/cancel") &&
	     strcmp(method, "session/prompt"))) {
		*errorp = acp_error(ctx, -32601, "method not found");
		return fy_invalid;
	}
	sid = fy_get(params, "sessionId", (const char *)NULL);
	if (!sid || strncmp(sid, "acp/", 4) ||
	    !fyai_branch_lookup(ctx->arena_branches, sid, &branch)) {
		*errorp = acp_error(ctx, -32602, "unknown ACP session");
		return fy_invalid;
	}
	s = acp_session_find(server, sid);
	if (!strcmp(method, "session/resume") ||
	    (server->version == 1 && !strcmp(method, "session/load")))
		return acp_session_resume(server, s, &branch, sid, method,
			params, errorp);
	if (!s || !s->open) {
		*errorp = acp_error(ctx, -32602, "session is not open");
		return fy_invalid;
	}
	if (!strcmp(method, "session/close"))
		return acp_session_stop(server, s, true, errorp);
	if (!strcmp(method, "session/cancel"))
		return acp_session_stop(server, s, false, errorp);
	return acp_session_prompt(server, conn, s, params, id, errorp);
}

static void acp_server_cleanup(struct acp_server *server)
{
	struct acp_session *s, *next;

	for (s = server->sessions; s; s = next) {
		next = s->next;
		if (s->pid > 0)
			(void)kill(s->pid, SIGTERM);
		acp_worker_release(s);
		free(s->id);
		free(s->cwd);
		free(s);
	}
	server->sessions = NULL;
}

static int acp_server_run(struct fyai_ctx *ctx)
{
	struct acp_server server = { .ctx = ctx };
	struct acp_session *s;
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	int rc = -1;

	if (!el)
		return -1;
	server.client = jsonrpc_conn_stdio(ctx, STDOUT_FILENO, STDIN_FILENO,
					0, "acp", NULL);
	if (!server.client ||
	    jsonrpc_conn_serve(server.client, acp_serve, &server))
		goto out;
	while (!jsonrpc_conn_closed(server.client)) {
		if (fyai_event_loop_step(el, -1) < 0)
			goto out;
		for (s = server.sessions; s; s = s->next)
			if (s->worker && !s->busy)
				acp_worker_release(s);
	}
	rc = 0;
out:
	acp_server_cleanup(&server);
	jsonrpc_conn_destroy(server.client);
	return rc;
}

static int acp_worker_update(struct fyai_ctx *ctx, fy_generic update)
{
	if (!ctx->acp_rpc)
		return 0;
	return jsonrpc_notify(ctx->acp_rpc, "acp/update",
		fy_mapping(fyai_ctx_transient_gb(ctx), "update", update));
}

void fyai_acp_worker_text(struct fyai_ctx *ctx, const char *text, size_t len)
{
	struct fy_generic_builder *gb;
	char *copy;

	if (!ctx || !ctx->acp_rpc || !ctx->acp_agent_message_id || !len)
		return;
	copy = malloc(len + 1);
	if (!copy)
		return;
	memcpy(copy, text, len);
	copy[len] = '\0';
	gb = fyai_ctx_transient_gb(ctx);
	(void)acp_worker_update(ctx, fy_mapping(gb,
		"sessionUpdate", "agent_message_chunk",
		"messageId", ctx->acp_agent_message_id,
		"content", fy_mapping(gb, "type", "text", "text", copy)));
	free(copy);
}

int fyai_acp_worker_tool(struct fyai_ctx *ctx, const char *tool, bool ok,
			 const char *cause)
{
	struct fy_generic_builder *gb;
	char id[64];

	if (!ctx || !ctx->acp_rpc || !tool)
		return 0;
	if (!acp_random_id(id, sizeof(id), "tool_"))
		return -1;
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return -1;
	return acp_worker_update(ctx, fy_mapping(gb,
		"sessionUpdate", "tool_call_update", "toolCallId", id,
		"name", tool, "title", tool,
		"status", ok ? "completed" : "failed",
		"content", cause && *cause ?
			fy_sequence(gb, fy_mapping(gb, "type", "content",
				"content", fy_mapping(gb, "type", "text", "text", cause))) :
			fy_seq_empty));
}

static fy_generic acp_worker_serve(struct jsonrpc_conn *conn,
				   const char *method, fy_generic params,
				   fy_generic id, void *userdata,
				   fy_generic *errorp)
{
	struct acp_worker *worker = userdata;
	struct fyai_ctx *ctx = worker->ctx;
	const char *text, *mid, *aid;

	if (!strcmp(method, "acp/cancel")) {
		worker->cancelled = true;
		ctx->interrupt_pending = true;
		return fy_invalid;
	}
	if (strcmp(method, "acp/run")) {
		*errorp = acp_error(ctx, -32601, "method not found");
		return fy_invalid;
	}
	if (worker->ran || worker->pending) {
		*errorp = acp_error(ctx, -32003, "worker is busy");
		return fy_invalid;
	}
	text = fy_get(params, "text", (const char *)NULL);
	mid = fy_get(params, "messageId", (const char *)NULL);
	aid = fy_get(params, "agentId", (const char *)NULL);
	if (!text || !mid || !aid || !fy_is_sequence(fy_get(params, "content"))) {
		*errorp = acp_error(ctx, -32602, "invalid ACP turn");
		return fy_invalid;
	}
	worker->text = strdup(text);
	worker->message_id = strdup(mid);
	worker->agent_id = strdup(aid);
	worker->content = fy_gb_internalize(ctx->cfg->gb,
		fy_get(params, "content", fy_invalid));
	worker->run_id = fy_gb_internalize(ctx->cfg->gb, id);
	if (!worker->text || !worker->message_id || !worker->agent_id ||
	    !fy_is_valid(worker->content) || !fy_is_valid(worker->run_id)) {
		*errorp = acp_error(ctx, -32000, "cannot retain ACP turn");
		return fy_invalid;
	}
	worker->pending = true;
	jsonrpc_conn_defer(conn);
	return fy_invalid;
}

static int acp_worker_turn(struct acp_worker *worker)
{
	struct fyai_ctx *ctx = worker->ctx;
	struct fy_generic_builder *gb;
	fy_generic user, turn;

	if (fyai_setup_transient_builder(ctx))
		return -1;
	gb = fyai_ctx_transient_gb(ctx);
	user = fyai_make_instruction_message(ctx, worker->text);
	user = fy_assoc(gb, user, "acp_message_id", worker->message_id);
	user = fy_assoc(gb, user, "acp_content", worker->content);
	turn = fyai_turn_append(ctx, ctx->last_message, fy_sequence(gb, user));
	turn = fyai_output_record(ctx, turn, FYAI_OUTPUT_USER, worker->text);
	if (fy_is_invalid(turn))
		return -1;
	ctx->last_message = fy_gb_internalize(ctx->gb, turn);
	if (fy_is_invalid(ctx->last_message) || fyai_publish_state(ctx))
		return -1;
	if (jsonrpc_notify(worker->conn, "acp/inserted",
		fy_mapping(gb, "messageId", worker->message_id)))
		return -1;
	if (acp_worker_update(ctx, fy_mapping(gb,
		"sessionUpdate", "user_message",
		"messageId", worker->message_id,
		"content", worker->content)))
		return -1;
	if (acp_worker_update(ctx, fy_mapping(gb,
		"sessionUpdate", "state_update", "state", "running")))
		return -1;
	if (acp_worker_update(ctx, fy_mapping(gb,
		"sessionUpdate", "agent_message",
		"messageId", worker->agent_id,
		"content", fy_seq_empty)))
		return -1;
	ctx->acp_agent_message_id = worker->agent_id;
	turn = fyai_run_turn(ctx, ctx->last_message);
	ctx->acp_agent_message_id = NULL;
	turn = fyai_report_diag(ctx, turn);
	if (fy_is_invalid(turn))
		return -1;
	ctx->last_message = turn;
	return fyai_publish_state(ctx);
}

static int acp_worker_run(struct fyai_ctx *ctx)
{
	struct acp_worker worker = { .ctx = ctx, .content = fy_invalid };
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	struct fy_generic_builder *gb;
	fy_generic reply, error;
	int rc = -1, turn_rc;

	if (!el)
		return -1;
	worker.conn = jsonrpc_conn_stdio(ctx, STDOUT_FILENO, STDIN_FILENO,
					0, "acp-worker", NULL);
	if (!worker.conn ||
	    jsonrpc_conn_serve(worker.conn, acp_worker_serve, &worker))
		goto out;
	ctx->acp_rpc = worker.conn;
	while (!worker.done && !jsonrpc_conn_closed(worker.conn)) {
		if (worker.pending) {
			worker.pending = false;
			worker.ran = true;
			turn_rc = acp_worker_turn(&worker);
			gb = fyai_ctx_transient_gb(ctx);
			fyai_error_check(ctx, gb, out,
				 "could not acquire ACP worker storage");
			reply = turn_rc ? fy_invalid : fy_mapping(gb, "ok", true);
			error = turn_rc ? acp_error(ctx, -32000,
				"ACP turn failed") : fy_invalid;
			fyai_error_check(ctx,
				!jsonrpc_conn_respond(worker.conn, worker.run_id,
						      reply, error),
				out, "could not send ACP worker result");
			worker.done = true;
			continue;
		}
		if (fyai_event_loop_step(el, -1) < 0)
			goto out;
	}
	while (jsonrpc_conn_has_output(worker.conn))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
	rc = 0;
out:
	ctx->acp_rpc = NULL;
	jsonrpc_conn_destroy(worker.conn);
	free(worker.text);
	free(worker.message_id);
	free(worker.agent_id);
	return rc;
}

int fyai_cmd_acp_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	cfg->interactive = false;
	cfg->protocol_stdout = true;
	cfg->acp_worker = fy_get(args, "worker", false);
	return 0;
}

int fyai_cmd_acp(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return call->ctx->cfg->acp_worker ?
		acp_worker_run(call->ctx) : acp_server_run(call->ctx);
}
