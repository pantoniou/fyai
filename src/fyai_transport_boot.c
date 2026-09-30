/*
 * fyai_transport_boot.c - start the credential transport and become its client
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_INIT

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

#include "fyai.h"
#include "fyai_agent.h"
#include "fyai_event.h"
#include "fyai_secret.h"
#include "fyai_auth.h"
#include "fyai_transport.h"
#include "fyai_transport_boot.h"
#include "fyai_transport_cfg.h"
#include "fyai_transport_client.h"
#include "fyai_transport_ctl.h"
#include "fyai_transport_sock.h"
#include "utils.h"

extern char **environ;

static int ctx_call(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			    fy_generic req, int fd, fy_generic *reply, const char **why);

/* The execution id of the supervisor, which is also the root agent. */
#define ROOT_EXEC_ID 1
#define FYAI_CTL_DRAIN_BATCH 32

bool fyai_transport_supervised(void)
{
	const char *fd = getenv(FYAI_TRANSPORT_FD_ENV);

	return fd && *fd;
}

bool fyai_env_is_credential(const char *name)
{
	static const char *const suffixes[] = { "_API_KEY", "_APIKEY", NULL };
	size_t n = strlen(name), i;
	size_t s;

	/* The MCP client reads this one; it is not a model credential. */
	if (!strcasecmp(name, "MCP_API_KEY"))
		return false;
	for (i = 0; suffixes[i]; i++) {
		s = strlen(suffixes[i]);

		if (n > s && !strcasecmp(name + n - s, suffixes[i]))
			return true;
	}
	return false;
}

/* Remove the credentials of the model providers from the environment. */
static int env_scrub(const struct fyai_cfg *cfg)
{
	char name[256];
	const char *ref;
	size_t i, len;
	bool removed;
	int rc = 0;
	const char *eq, *end;
	size_t n;

	do {
		removed = false;
		for (i = 0; environ && environ[i]; i++) {
			eq = strchr(environ[i], '=');

			len = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
			if (len >= sizeof(name)) {
				rc = -1;
				continue;
			}
			memcpy(name, environ[i], len);
			name[len] = '\0';
			if (!fyai_env_is_credential(name))
				continue;
			if (unsetenv(name) < 0) {
				rc = -1;
				continue;
			}
			removed = true;
			break;
		}
	} while (removed);

	/* The variables that the credential source of this run names. */
	ref = cfg->api_key_ref;
	while (ref && *ref) {
		end = strchr(ref, '|');
		n = end ? (size_t)(end - ref) : strlen(ref);

		if (n > 4 && !strncmp(ref, "env:", 4) && n - 4 < sizeof(name)) {
			memcpy(name, ref + 4, n - 4);
			name[n - 4] = '\0';
			if (unsetenv(name) < 0)
				rc = -1;
		}
		ref = end ? end + 1 : NULL;
	}
	return rc;
}

/* The control conversation. */

struct boot {
	struct fyai_cfg *cfg;
	int ctl;
	long long seq;
	bool found;		/* the `found` field of the last ok reply */
	char error[256];
};

/* Wait for the reply to @req, skipping events. */
static int boot_call(struct boot *b, struct fy_generic_builder *gb,
		     fy_generic req, int fd, const char **why)
{
	struct pollfd pfd = { .fd = b->ctl, .events = POLLIN };
	fy_generic reply, op, m;
	int rc;

	rc = fyai_ctl_send(b->ctl, req, fd, 0);
	if (rc) {
		*why = "cannot send to the credential transport";
		return rc;
	}
	for (;;) {
		rc = poll(&pfd, 1, 30000);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0) {
			*why = "the credential transport did not answer";
			return rc < 0 ? -errno : -ETIMEDOUT;
		}
		rc = fyai_ctl_recv(b->ctl, gb, &reply, NULL);
		if (rc == -EAGAIN)
			continue;
		if (rc) {
			*why = "the credential transport exited before it was ready";
			return rc;
		}
		op = fy_get(reply, "op", fy_invalid);
		if (fy_equal(op, "event"))
			continue;
		if (fy_equal(op, "ok")) {
			b->found = fy_get(reply, "found", false);
			return 0;
		}
		if (fy_equal(op, "error")) {
			m = fy_get(reply, "message", fy_invalid);
			snprintf(b->error, sizeof(b->error), "the credential transport refused: %s",
				 fy_is_string(m) ? fy_castp(&m, "") : "no reason");
			*why = b->error;
			return -EPERM;
		}
		*why = "the credential transport sent an unexpected reply";
		return -EPROTO;
	}
}

static int boot_send(struct boot *b, fy_generic (*build)(struct boot *,
			 struct fy_generic_builder *, void *), void *arg, int fd,
		     const char **why)
{
	char storage[2 * FYAI_CTL_MAX + FY_GENERIC_BUILDER_LINEAR_IN_PLACE_MIN_SIZE];
	struct fy_generic_builder *gb;

	gb = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
						 storage, sizeof(storage));
	if (!gb) {
		*why = "out of memory";
		return -ENOMEM;
	}
	return boot_call(b, gb, build(b, gb, arg), fd, why);
}

struct admit_arg {
	struct fyai_transport_allow *allow;
	size_t n;
};

static fy_generic build_init(struct boot *b, struct fy_generic_builder *gb, void *arg)
{
	enum fyai_transport_level *level = arg;

	return fy_mapping(gb, "op", "init", "seq", ++b->seq,
			  "level", fyai_transport_level_name(*level),
			  "log", b->cfg->transport_logging,
			  "wire", b->cfg->wire_logging,
			  "whitewash", b->cfg->whitewash_api_keys);
}

static fy_generic build_credential(struct boot *b, struct fy_generic_builder *gb,
				   void *arg)
{
	(void)arg;
	return fy_mapping(gb, "op", "credential", "seq", ++b->seq, "name", "cli",
			  "value", b->cfg->api_key);
}

static fy_generic build_probe(struct boot *b, struct fy_generic_builder *gb,
			      void *arg)
{
	const char *source = arg;

	return fy_mapping(gb, "op", "probe", "seq", ++b->seq, "credential", source);
}

static fy_generic build_profiles(struct boot *b, struct fy_generic_builder *gb,
				 void *arg)
{
	return fy_mapping(gb, "op", "profiles", "seq", ++b->seq, "profiles",
			  fyai_ctl_profiles_encode(gb, arg));
}

static fy_generic build_admit(struct boot *b, struct fy_generic_builder *gb,
			      void *arg)
{
	struct admit_arg *a = arg;

	return fy_mapping(gb, "op", "admit", "seq", ++b->seq,
			  "id", (long long)ROOT_EXEC_ID, "parent", 0LL,
			  "pid", (long long)getpid(), "uid", (long long)getuid(),
			  "grant", fyai_ctl_grant_encode(gb, a->allow, a->n));
}

/* Start the transport: fork it and execute this program as `fyai transport`. */
static pid_t transport_spawn(struct fyai_cfg *cfg, int ctl_child, int ctl_parent)
{
	const char *argv[9];
	pid_t pid;
	int n = 0;

	pid = fork();
	if (pid)
		return pid;
	close(ctl_parent);
	argv[n++] = "fyai";
	argv[n++] = "--color";
	argv[n++] = "off";
	argv[n++] = "transport";
	argv[n++] = "--control-fd";
	argv[n++] = fy_sprintfa("%d", ctl_child);
	if (cfg->arena_dir && *cfg->arena_dir) {
		argv[n++] = "--arena";
		argv[n++] = cfg->arena_dir;
	}
	argv[n] = NULL;
	/* The child starts with the signal state of a new program. */
	{
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
	}
	fyai_exec_self(argv);
	_exit(127);
}

/* Copy @argv without the arguments that carry a key. */
static char **argv_without_key(int argc, char **argv)
{
	char **out = calloc((size_t)argc + 1, sizeof(*out));
	int i, n = 0;

	if (!out)
		return NULL;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--")) {
			while (i < argc)
				out[n++] = argv[i++];
			break;
		}
		if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "--api-key")) {
			i++;			/* and its value */
			continue;
		}
		if (!strncmp(argv[i], "--api-key=", 10) ||
		    (argv[i][0] == '-' && argv[i][1] == 'k'))
			continue;
		out[n++] = argv[i];
	}
	out[n] = NULL;
	return out;
}

/* Give the next image the stdin prompt that this one consumed. */
static int stdin_restore(const struct fyai_cfg *cfg)
{
#ifdef __linux__
	size_t len, done = 0;
	int fd;
	ssize_t n;

	if (!cfg->stdin_consumed)
		return 0;
	len = cfg->prompt ? strlen(cfg->prompt) : 0;
	fd = memfd_create("fyai-stdin", 0);
	if (fd < 0)
		return -1;
	while (done < len) {
		n = write(fd, cfg->prompt + done, len - done);

		if (n <= 0) {
			close(fd);
			return -1;
		}
		done += n;
	}
	if (lseek(fd, 0, SEEK_SET) < 0 || dup2(fd, STDIN_FILENO) < 0) {
		close(fd);
		return -1;
	}
	if (fd != STDIN_FILENO)
		close(fd);
	return 0;
#else
	(void)cfg;
	return 0;
#endif
}

static void transport_kill(pid_t pid)
{
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

static int transport_sanitize(struct fyai_cfg *cfg, int argc, char **argv,
			      char ***nargv)
{
	int rc;

	rc = env_scrub(cfg);
	fyai_cfg_error_check(cfg, !rc, err,
			     "credential isolation: cannot remove the credentials "
			     "from the environment");
	*nargv = argv_without_key(argc, argv);
	fyai_cfg_error_check(cfg, *nargv, err,
			     "credential isolation: cannot prepare the next image");
	rc = stdin_restore(cfg);
	fyai_cfg_error_check(cfg, !rc, err,
			     "credential isolation: cannot prepare the next image");
	return 0;
err:
	return -1;
}

static int fyai_credential_isolation_available(struct fyai_cfg *cfg,
						enum fyai_transport_level level)
{
#ifndef __linux__
	fyai_cfg_error(cfg, "credential isolation needs Linux");
	return -1;
#else
	fyai_cfg_error_check(cfg, !self_is_valgrinded(), err,
			     "credential isolation cannot run under valgrind: "
			     "the program cannot execute itself there");
	fyai_cfg_error_check(cfg, fyai_exec_self_available(), err,
			     "credential isolation cannot execute this program again");
	fyai_cfg_error_check(cfg, level != FYAI_TL_A, err,
			     "level-a needs the cgroup setup, which is not available yet; "
			     "use level-b or auto");
	return 0;
err:
	return -1;
#endif
}

const char *fyai_transport_effective_level(const struct fyai_ctx *ctx)
{
	const char *level;

	if (!ctx->tclient)
		return "none";
	level = getenv(FYAI_TRANSPORT_ISOLATION_ENV);
	return level && *level ? level : "on";
}

void fyai_transport_status_text(struct fyai_ctx *ctx, char *buf, size_t size)
{
	char storage[4 * FYAI_CTL_MAX + FY_GENERIC_BUILDER_LINEAR_IN_PLACE_MIN_SIZE];
	const char *req = getenv(FYAI_TRANSPORT_REQUESTED_ENV);
	const char *level = fyai_transport_effective_level(ctx);
	struct fy_generic_builder *gb;
	char asked[64] = "";
	fy_generic reply, lv;
	const char *why = "cannot create the transport status builder";
	int rc;

	if (!ctx->tclient) {
		snprintf(buf, size, "none");
		return;
	}
	gb = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
					      storage, sizeof(storage));
	if (gb) {
		rc = ctx_call(ctx, gb, fy_mapping(gb, "op", "status",
				"seq", ++ctx->transport_seq), -1, &reply, &why);
		if (!rc) {
			lv = fy_get(reply, "level", fy_invalid);
			if (fy_is_string(lv))
				level = fy_gb_intern_string(gb, fy_castp(&lv, ""));
			if (req && *req && strcmp(req, level))
				snprintf(asked, sizeof(asked), "requested %s; ", req);
			snprintf(buf, size, "%s (%stransport pid %lld; execution %llu of %zu; "
				 "%zu profiles; %lld in flight)", level, asked,
				 (long long)fy_get(reply, "pid", 0LL),
				 (unsigned long long)ctx->transport_exec,
				 (size_t)fy_len(fy_get(reply, "executions", fy_seq_empty)),
				 (size_t)fy_len(fy_get(reply, "profiles", fy_seq_empty)),
				 (long long)fy_get(reply, "active", 0LL));
			return;
		}
	}
	if (req && *req && strcmp(req, level))
		snprintf(asked, sizeof(asked), "requested %s; ", req);
	snprintf(buf, size, "%s (%stransport not answering: %s; process %ld, "
		 "channel %s)", level, asked, why, (long)ctx->transport_pid,
		 fyai_tclient_alive(ctx->tclient) ? "up" : "down");
}

static inline bool transport_wants_chatgpt(const struct fyai_cfg *cfg)
{
	const char *why = NULL;

	return cfg->auth_mode == FYAI_AUTH_CHATGPT ||
	       (cfg->auth_mode == FYAI_AUTH_AUTO && !cfg->no_auth &&
		fy_str_empty(cfg->api_key) && !fyai_auth_chatgpt_eligible(cfg, &why));
}

int fyai_transport_bootstrap(struct fyai_cfg *cfg, int argc, char **argv)
{
	struct fyai_transport_grant grant = { 0 };
	struct fyai_transport_allow allow[FYAI_TPC_KINDS];
	char names[FYAI_TPC_KINDS][FYAI_TPC_NAME_MAX];
	enum fyai_transport_level level;
	struct admit_arg admit;
	struct boot b = { .cfg = cfg };
	const char *why = NULL;
	bool login = false, has_credential;
	char **nargv = NULL, fdbuf[16];
	int ctl[2] = { -1, -1 }, agent[2] = { -1, -1 };
	pid_t pid = -1;
	int rc;
	const char *over = getenv(FYAI_TRANSPORT_ISOLATION_ENV);
	const char *requested;

	/* The environment sets the level for one run; a test can turn it on. */
	if (over && *over)
		cfg->agent_transport_isolation = fy_gb_intern_string(cfg->gb, over);
	if (fyai_transport_supervised() || cfg->tool_exec ||
	    !fyai_cfg_makes_requests(cfg))
		return 0;
	requested = cfg->agent_transport_isolation;
	rc = fyai_transport_level_parse(cfg->agent_transport_isolation, &level);
	fyai_cfg_error_check(cfg, !rc, err,
			     "agent/transport_isolation: '%s' is not none, auto, "
			     "level-a, or level-b", cfg->agent_transport_isolation);
	if (level == FYAI_TL_NONE)
		return 0;
	/* The host is asked for the best it has; a level that it lacks is an error. */
	if (level == FYAI_TL_AUTO)
		level = FYAI_TL_B;
	rc = fyai_credential_isolation_available(cfg, level);
	if (rc)
		goto err;

	/* The transport resolves the login source without exposing its tokens. */
	if (transport_wants_chatgpt(cfg)) {
		rc = fyai_auth_chatgpt_eligible(cfg, &why);
		fyai_cfg_error_check(cfg, !rc, err, "%s", why);
		cfg->api_key_ref = FYAI_AUTH_CHATGPT_REF;
		cfg->chatgpt_auth = true;
		cfg->api_url = fyai_auth_chatgpt_url();
		login = true;
	} else {
		has_credential = cfg->no_auth || !fy_str_empty(cfg->api_key);
		fyai_cfg_error_check(cfg, has_credential,
				     err, "no API key: credential isolation needs "
				     "an API key, a login, or a provider that takes none");
	}

	rc = fyai_transport_profiles_add(&grant, cfg, &why);
	if (rc) {
		fyai_cfg_error(cfg, "credential isolation: %s", why);
		return -1;
	}
	admit.allow = allow;
	admit.n = fyai_transport_allow_for(cfg, names, allow);

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctl) ||
	    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, agent)) {
		fyai_cfg_error(cfg, "credential isolation: cannot create the "
			       "channels: %s", strerror(errno));
		goto err;
	}
	b.ctl = ctl[0];
	pid = transport_spawn(cfg, ctl[1], ctl[0]);
	if (pid < 0) {
		fyai_cfg_error(cfg, "credential isolation: cannot start the "
			       "transport: %s", strerror(errno));
		goto err;
	}
	close(ctl[1]);
	ctl[1] = -1;

	rc = boot_send(&b, build_init, &level, -1, &why);
	if (!rc && login) {
		rc = boot_send(&b, build_probe, (void *)FYAI_AUTH_CHATGPT_REF, -1, &why);
		if (!rc && !b.found) {
			why = "no API key and no ChatGPT login: set a key or run `fyai auth login`";
			rc = -ENOENT;
		}
	}
	if (!rc && cfg->api_key_ref && !strcmp(cfg->api_key_ref, "mem:cli") &&
	    cfg->api_key && *cfg->api_key)
		rc = boot_send(&b, build_credential, NULL, -1, &why);
	if (!rc)
		rc = boot_send(&b, build_profiles, &grant, -1, &why);
	if (!rc)
		rc = boot_send(&b, build_admit, &admit, agent[1], &why);
	if (rc) {
		fyai_cfg_error(cfg, "credential isolation: %s", why);
		goto err;
	}
	close(agent[1]);
	agent[1] = -1;

	/* From here on, nothing of the credentials may reach the next image. */
	if (transport_sanitize(cfg, argc, argv, &nargv))
		goto err;
	fcntl(agent[0], F_SETFD, 0);
	fcntl(ctl[0], F_SETFD, 0);
	snprintf(fdbuf, sizeof(fdbuf), "%d", agent[0]);
	setenv(FYAI_TRANSPORT_FD_ENV, fdbuf, 1);
	snprintf(fdbuf, sizeof(fdbuf), "%d", ctl[0]);
	setenv(FYAI_TRANSPORT_CTL_ENV, fdbuf, 1);
	snprintf(fdbuf, sizeof(fdbuf), "%d", ROOT_EXEC_ID);
	setenv(FYAI_TRANSPORT_EXEC_ENV, fdbuf, 1);
	snprintf(fdbuf, sizeof(fdbuf), "%ld", (long)pid);
	setenv(FYAI_TRANSPORT_PID_ENV, fdbuf, 1);
	snprintf(fdbuf, sizeof(fdbuf), "%ld", (long)getpid());
	setenv(FYAI_TRANSPORT_OWNER_ENV, fdbuf, 1);
	/*
	 * The next image names the profiles from its configuration, and it no
	 * longer has the --api-key that made the source: say where the key is.
	 */
	if (cfg->api_key_ref)
		setenv(FYAI_TRANSPORT_KEYREF_ENV, cfg->api_key_ref, 1);
	/* What was asked for, so that `auto` can be told from what it became. */
	setenv(FYAI_TRANSPORT_REQUESTED_ENV, requested, 1);
	/* The next image finds the level from the environment, not the key. */
	setenv(FYAI_TRANSPORT_ISOLATION_ENV, fyai_transport_level_name(level), 1);

	fyai_transport_grant_clear(&grant);
	fyai_exec_self((const char *const *)nargv);
	fyai_cfg_error(cfg, "credential isolation: cannot execute this program "
		       "again: %s", strerror(errno));
err:
	free(nargv);
	fyai_transport_grant_clear(&grant);
	if (pid > 0)
		transport_kill(pid);
	for (rc = 0; rc < 2; rc++) {
		if (ctl[rc] >= 0)
			close(ctl[rc]);
		if (agent[rc] >= 0)
			close(agent[rc]);
	}
	return -1;
}

/* The supervisor. */

/*
 * Drain the control channel. The transport answers each request and reports
 * events; nobody waits for them here, so a full socket must not build up.
 */
static enum fyai_event_action ctl_drain(const struct fyai_event *ev)
{
	struct fyai_ctx *ctx = ev->userdata;
	char storage[2 * FYAI_CTL_MAX + FY_GENERIC_BUILDER_LINEAR_IN_PLACE_MIN_SIZE];
	struct fy_generic_builder *gb;
	fy_generic m, e, d;
	unsigned int n;
	int rc;

	gb = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
						 storage, sizeof(storage));
	if (!gb)
		return FYAIEA_CONTINUE;
	for (n = 0; n < FYAI_CTL_DRAIN_BATCH; n++) {
		fy_generic_builder_reset(gb);
		rc = fyai_ctl_recv(ctx->transport_ctl, gb, &m, NULL);
		if (!rc && fy_equal(fy_get(m, "op", fy_invalid), "event")) {
			e = fy_get(m, "event", fy_invalid);
			d = fy_get(m, "detail", fy_invalid);
			fyai_diag_tracef("transport", "%s: %s",
					 fy_is_string(e) ? fy_castp(&e, "") : "event",
					 fy_is_string(d) ? fy_castp(&d, "") : "");
		}
		if (rc == -EAGAIN)
			return FYAIEA_CONTINUE;
		if (rc) {
			/* The transport is gone; the next request reports it. */
			if (ctx->transport_src) {
				fyai_event_source_remove(ctx->transport_src);
				ctx->transport_src = NULL;
			}
			return FYAIEA_CONTINUE;
		}
	}
	return FYAIEA_CONTINUE;
}

/*
 * One request on the control connection of this process, and its reply. The
 * wait is a poll on the descriptor, not a nested event loop. This process has
 * one thread, so the drain source cannot run while the call waits and cannot
 * take the reply. Calls occur only for setup, configuration changes, child
 * admission, explicit status, and credential checks; transfers use the client.
 */
static int ctx_call(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		    fy_generic req, int fd, fy_generic *reply, const char **why)
{
	struct pollfd pfd = { .fd = ctx->transport_ctl, .events = POLLIN };
	fy_generic op, m;
	int rc;

	*why = NULL;
	if (ctx->transport_ctl <= STDERR_FILENO) {
		*why = "this process has no control connection to the transport";
		return -ENOTCONN;
	}
	rc = fyai_ctl_send(ctx->transport_ctl, req, fd, 0);
	if (rc) {
		*why = "cannot send to the credential transport";
		return rc;
	}
	for (;;) {
		rc = poll(&pfd, 1, 30000);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0) {
			*why = "the credential transport did not answer";
			return rc < 0 ? -errno : -ETIMEDOUT;
		}
		rc = fyai_ctl_recv(ctx->transport_ctl, gb, reply, NULL);
		if (rc == -EAGAIN)
			continue;
		if (rc) {
			*why = "the credential transport is not available";
			return rc;
		}
		op = fy_get(*reply, "op", fy_invalid);
		if (fy_equal(op, "event"))
			continue;
		if (fy_equal(op, "ok"))
			return 0;
		m = fy_get(*reply, "message", fy_invalid);
		*why = fy_is_string(m) ? fy_gb_intern_string(gb, fy_castp(&m, "")) :
			"the credential transport refused without a reason";
		return -EPERM;
	}
}

static bool names_have(const struct fyai_ctx *ctx, const char *name)
{
	unsigned int i;

	for (i = 0; i < ctx->transport_nnames; i++)
		if (!strcmp(ctx->transport_names[i], name))
			return true;
	return false;
}

static void names_add(struct fyai_ctx *ctx, const char *name)
{
	if (names_have(ctx, name))
		return;
	if (ctx->transport_nnames >= ARRAY_SIZE(ctx->transport_names))
		return;
	snprintf(ctx->transport_names[ctx->transport_nnames++],
		 sizeof(ctx->transport_names[0]), "%s", name);
}

/* Most profile names one configuration and its personas need. */
#define ENSURE_MAX	FYAI_CTL_MAX_GRANT

/*
 * Add the profiles that @cfg needs to @grant and its names to @names, without
 * repeating a name that is there. A profile that the configuration cannot
 * have is left out, and @why says why.
 */
static int ensure_add(const struct fyai_cfg *cfg, struct fyai_transport_grant *grant,
		      char names[][FYAI_TPC_NAME_MAX], size_t *n, const char **why)
{
	char one[FYAI_TPC_KINDS][FYAI_TPC_NAME_MAX];
	struct fyai_transport_allow allow[FYAI_TPC_KINDS];
	size_t i, j, cnt;
	int rc;

	rc = fyai_transport_profiles_add(grant, cfg, why);
	if (rc)
		return rc;
	cnt = fyai_transport_allow_for(cfg, one, allow);
	for (i = 0; i < cnt; i++) {
		for (j = 0; j < *n; j++)
			if (!strcmp(names[j], one[i]))
				break;
		if (j == *n && *n < ENSURE_MAX)
			snprintf(names[(*n)++], FYAI_TPC_NAME_MAX, "%.*s",
				 FYAI_TPC_NAME_MAX - 1, one[i]);
	}
	return 0;
}

/*
 * Add the profiles of every configured persona. A persona that does not
 * resolve is left out: a sub-agent that uses it fails when it starts, and an
 * unused persona must not fail this run.
 */
static void ensure_personas(struct fyai_ctx *ctx, struct fyai_transport_grant *grant,
			    char names[][FYAI_TPC_NAME_MAX], size_t *n)
{
	fy_generic personas = fy_get(fy_get(ctx->cfg->config_doc, "agent"),
				     "personas", fy_invalid);
	fy_generic key, persona;
	bool had_error = fyai_diag_got_error(&ctx->cfg->diag);
	struct fyai_cfg pc;
	const char *why;

	if (!fy_is_mapping(personas))
		return;
	fy_foreach_key_value(key, persona, personas) {
		if (!fy_is_mapping(persona))
			continue;
		if (fyai_agent_persona_cfg(ctx, persona, false, &pc)) {
			if (!had_error)
				fyai_diag_reset(&ctx->cfg->diag);
			continue;
		}
		(void)ensure_add(&pc, grant, names, n, &why);
	}
}

int fyai_transport_ensure(struct fyai_ctx *ctx)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fyai_transport_grant grant = { 0 };
	struct fyai_transport_allow allow[ENSURE_MAX];
	char names[ENSURE_MAX][FYAI_TPC_NAME_MAX];
	struct fy_generic_builder *gb = NULL;
	fy_generic reply;
	const char *why = NULL;
	size_t nneed = 0, i, n = 0;
	struct fyai_transport_allow root[FYAI_TPC_KINDS];
	char rn[FYAI_TPC_KINDS][FYAI_TPC_NAME_MAX];
	size_t cnt;
	bool all;
	int rc;

	/*
	 * Only the supervisor states profiles: it is the user session, and the
	 * one that can be told to change the set. A sub-agent has the grant its
	 * parent gave it, and the transport refuses what that grant lacks.
	 */
	if (!ctx->tclient || !ctx->transport_owner ||
	    ctx->transport_ctl <= STDERR_FILENO)
		return 0;
	/* The personas change with the configuration; the model can change without. */
	if (ctx->transport_stated && ctx->transport_gen == ctx->cfg->config_generation) {
		cnt = fyai_transport_allow_for(ctx->cfg, rn, root);
		all = true;

		for (i = 0; i < cnt; i++)
			all &= names_have(ctx, root[i].profile);
		if (all)
			return 0;
	}

	rc = ensure_add(ctx->cfg, &grant, names, &nneed, &why);
	fyai_error_check(ctx, !rc, err, "credential isolation: %s", why);
	ensure_personas(ctx, &grant, names, &nneed);
	gb = fy_generic_builder_create(&cfg);
	fyai_error_check(ctx, gb, err, "out of memory");

	rc = ctx_call(ctx, gb, fy_mapping(gb, "op", "profiles",
			"seq", ++ctx->transport_seq, "merge", true,
			"profiles", fyai_ctl_profiles_encode(gb, &grant)),
		       -1, &reply, &why);
	fyai_error_check(ctx, !rc, err, "credential isolation: %s", why);

	/* The grant lists every name this execution may use, old and new. */
	for (i = 0; i < ctx->transport_nnames && n < ENSURE_MAX; i++) {
		allow[n].profile = ctx->transport_names[i];
		allow[n++].model = NULL;
	}
	for (i = 0; i < nneed && n < ENSURE_MAX; i++) {
		if (names_have(ctx, names[i]))
			continue;
		allow[n].profile = names[i];
		allow[n++].model = NULL;
	}
	rc = ctx_call(ctx, gb, fy_mapping(gb, "op", "grant",
			"seq", ++ctx->transport_seq,
			"id", (long long)ctx->transport_exec,
			"grant", fyai_ctl_grant_encode(gb, allow, n)),
		       -1, &reply, &why);
	fyai_error_check(ctx, !rc, err, "credential isolation: %s", why);
	for (i = 0; i < nneed; i++)
		names_add(ctx, names[i]);
	ctx->transport_gen = ctx->cfg->config_generation;
	ctx->transport_stated = true;
	fy_generic_builder_destroy(gb);
	fyai_transport_grant_clear(&grant);
	return 0;
err:
	if (gb)
		fy_generic_builder_destroy(gb);
	fyai_transport_grant_clear(&grant);
	return -1;
}

int fyai_transport_admit_child(struct fyai_ctx *ctx, pid_t pid, int agent_fd,
			       int ctl_fd, uint64_t *exec_id)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fyai_transport_allow allow[8];
	struct fy_generic_builder *gb;
	fy_generic reply;
	const char *why;
	unsigned int i;
	int rc;

	gb = fy_generic_builder_create(&cfg);
	fyai_error_check(ctx, gb, err, "out of memory");
	for (i = 0; i < ctx->transport_nnames; i++) {
		allow[i].profile = ctx->transport_names[i];
		allow[i].model = NULL;
	}
	rc = ctx_call(ctx, gb, fy_mapping(gb, "op", "admit",
			"seq", ++ctx->transport_seq,
			"id", 0LL, "parent", (long long)ctx->transport_exec,
			"pid", (long long)pid, "uid", (long long)getuid(),
			"grant", fyai_ctl_grant_encode(gb, allow,
						       ctx->transport_nnames)),
		       agent_fd, &reply, &why);
	fyai_error_check(ctx, !rc, err_gb,
			 "credential isolation: cannot register the sub-agent: %s", why);
	*exec_id = fy_get(reply, "id", 0LL);
	fyai_error_check(ctx, *exec_id, err_gb,
			 "credential isolation: the transport gave no execution id");
	rc = ctx_call(ctx, gb, fy_mapping(gb, "op", "ctl",
			"seq", ++ctx->transport_seq, "id", (long long)*exec_id), ctl_fd, &reply, &why);
	fyai_error_check(ctx, !rc, err_gb,
			 "credential isolation: cannot give the sub-agent a control "
			 "connection: %s", why);
	fy_generic_builder_destroy(gb);
	return 0;
err_gb:
	fy_generic_builder_destroy(gb);
err:
	return -1;
}

fy_generic fyai_transport_spawn_state(struct fyai_ctx *ctx,
				      struct fy_generic_builder *gb,
				      uint64_t exec_id)
{
	fy_generic names = fy_seq_empty;
	unsigned int i;

	for (i = 0; i < ctx->transport_nnames; i++)
		names = fy_append(gb, names, fy_value(gb, ctx->transport_names[i]));
	return fy_gb_mapping(gb, "exec", (long long)exec_id, "names", names);
}

int fyai_transport_child_attach(struct fyai_ctx *ctx, fy_generic state)
{
	uint64_t exec_id = fy_get(state, "exec", 0LL);
	fy_generic name;

	fyai_error_check(ctx, exec_id &&
			 fcntl(FYAI_TRANSPORT_CHILD_AGENT_FD, F_GETFD) >= 0 &&
			 fcntl(FYAI_TRANSPORT_CHILD_CTL_FD, F_GETFD) >= 0, err,
			 "the credential transport channels of this sub-agent are not open");
	ctx->tclient = fyai_tclient_open(ctx, FYAI_TRANSPORT_CHILD_AGENT_FD, exec_id);
	fyai_error_check(ctx, ctx->tclient, err,
			 "cannot open the credential transport client");
	ctx->transport_exec = exec_id;
	/* What the parent granted this child: it can grant no more to its own. */
	fy_foreach(name, fy_get(state, "names", fy_invalid)) {
		if (fy_is_string(name))
			names_add(ctx, fy_castp(&name, ""));
	}
	ctx->transport_ctl = FYAI_TRANSPORT_CHILD_CTL_FD;
	(void)fyai_event_add_fd(fyai_ctx_loop(ctx), ctx->transport_ctl, FYAIEV_READ,
				ctl_drain, ctx, &ctx->transport_src);
	ctx->transport_owner = false;
	return 0;
err:
	return -1;
}

bool fyai_transport_have_credential(struct fyai_ctx *ctx, const struct fyai_cfg *cfg)
{
	struct fy_generic_builder_cfg gcfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	char src[512];
	fy_generic reply;
	bool found = false;
	const char *why;

	if (cfg->no_auth || (cfg->api_key && *cfg->api_key))
		return true;
	if (!ctx->tclient || !fyai_transport_credential_source(cfg, src, sizeof(src)))
		return false;
	gb = fy_generic_builder_create(&gcfg);
	if (!gb)
		return false;
	if (!ctx_call(ctx, gb, fy_mapping(gb, "op", "probe",
			"seq", ++ctx->transport_seq, "credential", src), -1, &reply, &why))
		found = fy_get(reply, "found", false);
	fy_generic_builder_destroy(gb);
	return found;
}

int fyai_transport_env_grant(struct fyai_ctx *ctx, const char *const *names)
{
	struct fy_generic_builder_cfg gcfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	fy_generic list = fy_seq_empty, reply;
	const char *why;
	size_t n = 0;
	int sv[2] = { -1, -1 };
	int rc;

	if (!ctx->tclient || !names || !*names)
		return 0;
	fyai_transport_env_release(ctx);
	gb = fy_generic_builder_create(&gcfg);
	fyai_error_check(ctx, gb, err, "out of memory");
	rc = fyai_transport_socketpair(sv);
	fyai_error_check(ctx, !rc, err_gb,
			 "credential isolation: cannot make the credential channel: %s",
			 strerror(errno));
	for (; *names && n < 16; names++, n++)
		list = fy_append(gb, list, fy_value(gb, *names));
	rc = ctx_call(ctx, gb, fy_mapping(gb, "op", "envgrant",
			"seq", ++ctx->transport_seq, "names", list), sv[1], &reply, &why);
	close(sv[1]);
	sv[1] = -1;
	fyai_error_check(ctx, !rc, err_gb,
			 "credential isolation: cannot get the credentials of the "
			 "configured command: %s", why);
	fy_generic_builder_destroy(gb);
	ctx->transport_envfd = sv[0];
	return 0;
err_gb:
	fy_generic_builder_destroy(gb);
err:
	if (sv[0] >= 0)
		close(sv[0]);
	if (sv[1] >= 0)
		close(sv[1]);
	return -1;
}

void fyai_transport_env_take(struct fyai_ctx *ctx)
{
	char buf[65536];
	char *eq;
	ssize_t len;
	size_t off;

	if (ctx->transport_envfd <= STDERR_FILENO)
		return;
	len = recv(ctx->transport_envfd, buf, sizeof(buf) - 1, 0);
	close(ctx->transport_envfd);
	ctx->transport_envfd = 0;
	if (len <= 0)
		return;
	buf[len] = '\0';
	/* NAME=VALUE, each closed by a NUL. */
	for (off = 0; off < (size_t)len; off += strlen(buf + off) + 1) {
		eq = strchr(buf + off, '=');

		if (!eq || eq == buf + off)
			continue;
		*eq = '\0';
		if (setenv(buf + off, eq + 1, 1) < 0)
			fyai_error(ctx, "could not restore %s in the environment: %s",
				   buf + off, strerror(errno));
	}
	fyai_secret_clear(buf, sizeof(buf));
}

void fyai_transport_env_release(struct fyai_ctx *ctx)
{
	if (ctx->transport_envfd > STDERR_FILENO)
		close(ctx->transport_envfd);
	ctx->transport_envfd = 0;
}

void fyai_transport_sync_logging(struct fyai_ctx *ctx)
{
	int rc;

	if (!ctx->tclient || ctx->transport_ctl <= STDERR_FILENO)
		return;
	rc = fyai_ctl_send(ctx->transport_ctl,
			    fy_mapping("op", "log", "seq", 0LL,
				       "on", ctx->cfg->transport_logging,
				       "wire", ctx->cfg->wire_logging,
				       "whitewash", ctx->cfg->whitewash_api_keys),
			    -1, MSG_DONTWAIT);
	fyai_error_check(ctx, !rc, out,
			 "could not update transport logging: %s", strerror(-rc));
out:
	return;
}

int fyai_transport_attach(struct fyai_ctx *ctx)
{
	const char *fd = getenv(FYAI_TRANSPORT_FD_ENV);
	const char *exec = getenv(FYAI_TRANSPORT_EXEC_ENV);
	const char *ctl = getenv(FYAI_TRANSPORT_CTL_ENV);
	const char *pid = getenv(FYAI_TRANSPORT_PID_ENV);
	const char *owner = getenv(FYAI_TRANSPORT_OWNER_ENV);
	const char *keyref = getenv(FYAI_TRANSPORT_KEYREF_ENV);
	char names[FYAI_TPC_KINDS][FYAI_TPC_NAME_MAX];
	struct fyai_transport_allow allow[FYAI_TPC_KINDS];
	size_t n, i;
	int fdn, ctln;

	ctx->transport_ctl = -1;
	if (fy_str_empty(fd))
		return 0;
	/* Every image of the run names its profiles from the same source. */
	if (!fy_str_empty(keyref))
		ctx->cfg->api_key_ref = fy_gb_intern_string(ctx->cfg->gb, keyref);
	/* An executed child has its channels from its parent; see the spawn. */
	if (ctx->cfg->tool_exec)
		return 0;
	fdn = atoi(fd);
	ctln = ctl ? atoi(ctl) : -1;
	fyai_error_check(ctx, fdn > 2 && exec && fcntl(fdn, F_GETFD) >= 0, err,
			 "the credential transport channel is not open");
	ctx->tclient = fyai_tclient_open(ctx, fdn, exec ? strtoull(exec, NULL, 10) : 0);
	fyai_error_check(ctx, ctx->tclient, err,
			 "cannot open the credential transport client");
	if (ctln > 2 && fcntl(ctln, F_GETFD) >= 0) {
		ctx->transport_ctl = ctln;
		(void)fyai_event_add_fd(fyai_ctx_loop(ctx), ctln, FYAIEV_READ,
					ctl_drain, ctx, &ctx->transport_src);
	}
	ctx->transport_exec = exec ? strtoull(exec, NULL, 10) : 0;
	/* The bootstrap stated the profiles of this configuration. */
	n = fyai_transport_allow_for(ctx->cfg, names, allow);
	for (i = 0; i < n; i++)
		names_add(ctx, allow[i].profile);
	ctx->transport_pid = pid ? (pid_t)atol(pid) : 0;
	ctx->transport_owner = owner && (pid_t)atol(owner) == getpid();
	return 0;
err:
	return -1;
}

void fyai_transport_detach(struct fyai_ctx *ctx)
{

	if (!ctx->tclient)
		return;
	/*
	 * A reload executes this program again. The next image takes over the
	 * channels and the transport, which must keep running: drop the client and
	 * the drain source, and leave the descriptors and the process alone.
	 */
	if (ctx->cfg->reload_branch) {
		(void)fyai_tclient_release(ctx->tclient);
		ctx->tclient = NULL;
		if (ctx->transport_src) {
			fyai_event_source_remove(ctx->transport_src);
			ctx->transport_src = NULL;
		}
		ctx->transport_pid = 0;
		return;
	}
	fyai_tclient_close(ctx->tclient);
	ctx->tclient = NULL;
	if (ctx->transport_src) {
		fyai_event_source_remove(ctx->transport_src);
		ctx->transport_src = NULL;
	}
	if (ctx->transport_ctl >= 0) {
		if (ctx->transport_owner) {
			(void)fyai_ctl_send(ctx->transport_ctl,
				fy_mapping("op", "shutdown", "seq", 0LL),
				-1, MSG_DONTWAIT);
		}
		close(ctx->transport_ctl);
		ctx->transport_ctl = -1;
	}
	/* Only the process that started the transport reaps it. */
	if (ctx->transport_owner && ctx->transport_pid > 0) {
		kill(ctx->transport_pid, SIGTERM);
		waitpid(ctx->transport_pid, NULL, 0);
	}
	ctx->transport_pid = 0;
}
