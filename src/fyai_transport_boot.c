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
#include "fyai_event.h"
#include "fyai_secret.h"
#include "fyai_transport.h"
#include "fyai_transport_boot.h"
#include "fyai_transport_cfg.h"
#include "fyai_transport_client.h"
#include "fyai_transport_ctl.h"
#include "utils.h"

extern char **environ;

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

	/* The MCP client reads this one; it is not a model credential. */
	if (!strcasecmp(name, "MCP_API_KEY"))
		return false;
	for (i = 0; suffixes[i]; i++) {
		size_t s = strlen(suffixes[i]);

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

	do {
		removed = false;
		for (i = 0; environ && environ[i]; i++) {
			const char *eq = strchr(environ[i], '=');

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
		const char *end = strchr(ref, '|');
		size_t n = end ? (size_t)(end - ref) : strlen(ref);

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
		if (fy_equal(op, "ok"))
			return 0;
		if (fy_equal(op, "error")) {
			m = fy_get(reply, "message", fy_invalid);
			snprintf(b->error, sizeof(b->error),
				 "the credential transport refused: %s",
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

	if (!cfg->stdin_consumed)
		return 0;
	len = cfg->prompt ? strlen(cfg->prompt) : 0;
	fd = memfd_create("fyai-stdin", 0);
	if (fd < 0)
		return -1;
	while (done < len) {
		ssize_t n = write(fd, cfg->prompt + done, len - done);

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

int fyai_transport_bootstrap(struct fyai_cfg *cfg, int argc, char **argv)
{
	struct fyai_transport_grant grant = { 0 };
	struct fyai_transport_allow allow[FYAI_TPC_KINDS];
	char names[FYAI_TPC_KINDS][FYAI_TPC_NAME_MAX];
	enum fyai_transport_level level;
	struct admit_arg admit;
	struct boot b = { .cfg = cfg };
	const char *why = NULL;
	char **nargv = NULL, fdbuf[16];
	int ctl[2] = { -1, -1 }, agent[2] = { -1, -1 };
	pid_t pid = -1;
	int rc;
	const char *over = getenv(FYAI_TRANSPORT_ISOLATION_ENV);

	/* The environment sets the level for one run; a test can turn it on. */
	if (over && *over)
		cfg->agent_transport_isolation = fy_gb_intern_string(cfg->gb, over);
	if (fyai_transport_supervised() || cfg->tool_exec ||
	    !fyai_cfg_makes_requests(cfg))
		return 0;
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

	/*
	 * The key must be here now, in the image that may hold it. Without one,
	 * an unisolated run falls back to the ChatGPT login, whose tokens the
	 * agent would keep: that run cannot be isolated.
	 */
	fyai_cfg_error_check(cfg, cfg->auth_mode != FYAI_AUTH_CHATGPT &&
			     (cfg->no_auth || !fy_str_empty(cfg->api_key)), err,
			     "no API key: credential isolation needs an API key or "
			     "a provider that takes none; the ChatGPT login cannot run isolated");

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


void fyai_transport_sync_logging(struct fyai_ctx *ctx)
{
	int rc;

	if (!ctx->tclient || ctx->transport_ctl < 0)
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
	int fdn, ctln;

	ctx->transport_ctl = -1;
	if (fy_str_empty(fd) || ctx->cfg->tool_exec)
		return 0;
	if (!fy_str_empty(keyref))
		ctx->cfg->api_key_ref = fy_gb_intern_string(ctx->cfg->gb, keyref);
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
