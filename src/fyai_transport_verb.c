/*
 * fyai_transport_verb.c - the fyai transport verb
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The transport is the only process that reads a credential. It is started by
 * the supervisor with a control channel on a descriptor number that it states,
 * and it ends when that channel closes. See fyai_transport_ctl.h for the
 * protocol.
 *
 * Nothing here prints. The transport writes its log file and sends events on
 * the control channel; its standard streams belong to the supervisor.
 */

#define FYAI_MODULE FYAIEM_STREAM

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "fyai.h"
#include "fyai_cmd_int.h"
#include "fyai_event.h"
#include "fyai_secret.h"
#include "fyai_transport.h"
#include "fyai_transport_ctl.h"
#include "fyai_transport_server.h"
#include "utils.h"

/* Descriptor that the control channel is moved to. */
#define CTL_FD 3
#define CTL_WAKE_BATCH 32
#define CTL_BUILDER_SIZE (4 * FYAI_CTL_MAX + \
			  FY_GENERIC_BUILDER_LINEAR_IN_PLACE_MIN_SIZE)

struct mem_cred {
	char *name;
	char *value;
};

struct fyai_transport_verb {
	struct fyai_ctx *ctx;
	int ctl;
	struct fyai_event_source *src;
	struct fyai_transport_registry *reg;
	struct fyai_transport_server *srv;
	struct mem_cred *mem;
	size_t nmem;
	bool inited;
	bool fd_taken;		/* the request kept the descriptor it carried */
	volatile bool done;
};

/* Hardening, before anything is read. */

/*
 * Make the process unreadable to a process of the same user, close every
 * descriptor but the control channel, and leave the terminal. A step that
 * fails stops the transport: it must not run with the credentials of the user
 * in a process that another process of the user can read.
 */
int fyai_cmd_transport_early(struct fyai_cfg *cfg, fy_generic args)
{
#ifdef __linux__
	struct rlimit nocore = { 0, 0 };
	long long fd = fy_get(args, "control_fd", -1LL);
	fy_generic arena = fy_get(args, "arena", fy_invalid);
	int null, rc;

	fyai_cfg_error_check(cfg, fd >= CTL_FD && fd <= 1 << 20, err,
			     "transport: --control-fd names no descriptor");
	rc = prctl(PR_SET_DUMPABLE, 0);
	fyai_cfg_error_check(cfg, !rc, err,
			     "transport: cannot make the process undumpable: %s",
			     strerror(errno));
	rc = setrlimit(RLIMIT_CORE, &nocore);
	fyai_cfg_error_check(cfg, !rc, err,
			     "transport: cannot disable core dumps: %s",
			     strerror(errno));
	rc = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	fyai_cfg_error_check(cfg, !rc, err,
			     "transport: cannot set no-new-privileges: %s",
			     strerror(errno));
	rc = prctl(PR_SET_PDEATHSIG, SIGTERM);
	fyai_cfg_error_check(cfg, !rc && getppid() != 1, err,
			     "transport: supervisor is no longer running");

	/* An inherited descriptor is never a channel of this process. */
	fyai_cfg_error_check(cfg, fcntl((int)fd, F_GETFD) >= 0, err,
			     "transport: descriptor %lld is not open", fd);
	if (fd != CTL_FD) {
		rc = dup2((int)fd, CTL_FD);
		fyai_cfg_error_check(cfg, rc == CTL_FD, err,
				     "transport: cannot move the control channel: %s",
				     strerror(errno));
	}
	fyai_close_fds_from(CTL_FD + 1);
	fcntl(CTL_FD, F_SETFD, FD_CLOEXEC);

	null = open("/dev/null", O_RDONLY);
	if (null >= 0) {
		(void)dup2(null, STDIN_FILENO);
		if (null > STDERR_FILENO)
			close(null);
	}

	/* Terminal signals belong to the supervisor, not to this process. */
	if (setsid() < 0 && errno != EPERM)
		fyai_cfg_error(cfg, "transport: cannot leave the session: %s",
			       strerror(errno));
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	cfg->transport_ctl_fd = CTL_FD;
	cfg->interactive = false;
	if (fy_is_string(arena))
		cfg->arena_dir = fy_gb_intern_string(cfg->gb, fy_castp(&arena, ""));
	return 0;
err:
	return -1;
#else
	(void)args;
	fyai_cfg_error(cfg, "transport: credential isolation needs Linux");
	return -1;
#endif
}

/* Credentials. */

static const struct mem_cred *mem_find(const struct fyai_transport_verb *v, const char *name)
{
	size_t i;

	for (i = 0; i < v->nmem; i++)
		if (!strcmp(v->mem[i].name, name))
			return &v->mem[i];
	return NULL;
}

static int mem_set(struct fyai_transport_verb *v, const char *name, const char *value)
{
	struct mem_cred *m = (struct mem_cred *)mem_find(v, name), *nm;
	char *val = strdup(value), *nn;

	if (!val)
		return -ENOMEM;
	if (m) {
		if (m->value)
			fyai_secret_clear(m->value, strlen(m->value));
		free(m->value);
		m->value = val;
		return 0;
	}
	nn = strdup(name);
	if (!nn)
		goto err;
	nm = realloc(v->mem, (v->nmem + 1) * sizeof(*nm));
	if (!nm)
		goto err;
	v->mem = nm;
	v->mem[v->nmem].name = nn;
	v->mem[v->nmem].value = val;
	v->nmem++;
	return 0;
err:
		fyai_secret_clear(val, strlen(val));
		free(val);
		free(nn);
		return -ENOMEM;
}

/*
 * Resolve one credential source: "env:NAME", "secret:NAME" (the secret store,
 * as the api_key setting reads it), or "mem:NAME" (sent on the channel).
 * Return 0 and a malloc'd value, -ENOENT if the source holds none, or -EINVAL
 * for a source of another kind.
 */
static int cred_env(struct fyai_transport_verb *v, const char *name, char **secret)
{
	const char *value;

	(void)v;
	value = getenv(name);
	if (!value || !*value)
		return -ENOENT;
	*secret = strdup(value);
	return *secret ? 0 : -ENOMEM;
}

static int cred_mem(struct fyai_transport_verb *v, const char *name, char **secret)
{
	const struct mem_cred *m;

	m = mem_find(v, name);
	if (!m || !m->value || !*m->value)
		return -ENOENT;
	*secret = strdup(m->value);
	return *secret ? 0 : -ENOMEM;
}

static int cred_secret(struct fyai_transport_verb *v, const char *name, char **secret)
{
	char key[300], *found = NULL;
	size_t flen = 0;
	int rc, n;

	(void)v;
	n = snprintf(key, sizeof(key), "fyai:%s", name);
	if (n < 0 || (size_t)n >= sizeof(key))
		return -EINVAL;
	if (fyai_secret_kernel_get(key, &found, &flen) != FYAI_SECRET_OK)
		return -ENOENT;
	rc = found && *found ? 0 : -ENOENT;
	if (!rc) {
		*secret = strdup(found);
		if (!*secret)
			rc = -ENOMEM;
	}
	fyai_secret_clear_and_free(&found, &flen);
	return rc;
}

struct credential_source {
	const char *prefix;
	int (*get)(struct fyai_transport_verb *, const char *, char **);
};

static const struct credential_source credential_sources[] = {
	{ "env:", cred_env },
	{ "mem:", cred_mem },
	{ "secret:", cred_secret },
};

static int cred_one(struct fyai_transport_verb *v, const char *src, size_t len, char **secret)
{
	char tmp[256];
	size_t i, n;

	if (len >= sizeof(tmp))
		return -EINVAL;
	memcpy(tmp, src, len);
	tmp[len] = '\0';
	for (i = 0; i < sizeof(credential_sources) / sizeof(credential_sources[0]); i++) {
		n = strlen(credential_sources[i].prefix);
		if (!strncmp(tmp, credential_sources[i].prefix, n))
			return credential_sources[i].get(v, tmp + n, secret);
	}
	return -EINVAL;
}

/*
 * Resolve the credential of a profile. The source can be a chain, "A|B": the
 * first source that holds a value wins, as the provider default tries the
 * environment and then the secret store.
 */
static int verb_cred(void *ud, const struct fyai_transport_profile *pr,
		     char **secret)
{
	const char *src = pr->credential;
	const char *end;
	size_t len;
	int rc = -ENOENT;

	*secret = NULL;
	while (src && *src) {
		end = strchr(src, '|');
		len = end ? (size_t)(end - src) : strlen(src);

		rc = cred_one(ud, src, len, secret);
		if (!rc || rc == -ENOMEM)
			return rc;
		src = end ? end + 1 : NULL;
	}
	return rc == -EINVAL ? -EINVAL : -ENOENT;
}

/* Events go to the supervisor if it is listening; a full socket drops them. */
static void verb_event(void *ud, uint64_t id, const char *event,
		       const char *detail)
{
	char storage[4096];
	struct fyai_transport_verb *v = ud;
	struct fy_generic_builder *gb;
	int rc;

	gb = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
						 storage, sizeof(storage));
	if (!gb)
		return;
	rc = fyai_ctl_send(v->ctl,
			    fy_mapping(gb, "op", "event", "event", event,
				       "id", (long long)id,
				       "detail", detail ? detail : ""),
			    -1, MSG_DONTWAIT);
	if (rc == -EPIPE || rc == -ECONNRESET || rc == -ENOTCONN)
		v->done = true;
}

/* Requests. */

static int op_init(struct fyai_transport_verb *v, fy_generic m)
{
	enum fyai_transport_level level;
	struct fyai_transport_registry *reg;
	struct fyai_transport_server *srv;
	fy_generic lv = fy_get(m, "level", fy_invalid);
	fy_generic cg = fy_get(m, "cgroup", fy_invalid);

	if (v->inited)
		return -EALREADY;
	if (!fy_is_string(lv) ||
	    fyai_transport_level_parse(fy_castp(&lv, ""), &level) ||
	    (level != FYAI_TL_A && level != FYAI_TL_B))
		return -EINVAL;
	reg = fyai_transport_registry_create(level,
			level == FYAI_TL_A && fy_is_string(cg) ?
			fy_castp(&cg, "") : NULL);
	if (!reg)
		return -EINVAL;
	srv = fyai_transport_server_create(v->ctx, reg, verb_cred, v,
					   verb_event, v);
	if (!srv) {
		fyai_transport_registry_destroy(reg);
		return -ENOMEM;
	}
	v->reg = reg;
	v->srv = srv;
	v->ctx->cfg->transport_logging = fy_get(m, "log", false);
	v->ctx->cfg->wire_logging = fy_get(m, "wire", false);
	v->ctx->cfg->whitewash_api_keys = fy_get(m, "whitewash", true);
	v->inited = true;
	return 0;
}

static const char *errno_text(int rc)
{
	switch (rc) {
	case -EPERM:
		return "the process is not contained";
	case -EEXIST:
		return "the execution, process, or channel is already admitted";
	case -ENOENT:
		return "no such execution";
	case -EINVAL:
		return "the request is not valid";
	case -ENOMEM:
		return "out of memory";
	case -EALREADY:
		return "init was already done";
	case -EBADF:
		return "the channel descriptor is missing";
	default:
		return "the operation failed";
	}
}

static int op_admit(struct fyai_transport_verb *v, struct fy_generic_builder *gb,
		    fy_generic m, int fd)
{
	struct fyai_transport_allow allow[FYAI_CTL_MAX_GRANT];
	struct fyai_transport_ns_req ns;
	size_t n;
	int rc;

	if (fd < 0)
		return -EBADF;
	rc = fyai_ctl_grant_parse(gb, fy_get(m, "grant", fy_invalid), allow,
				  FYAI_CTL_MAX_GRANT, &n);
	if (rc)
		return rc;
	rc = fyai_ctl_ns_parse(fy_get(m, "ns", fy_invalid), &ns);
	if (rc)
		return rc;
	rc = fyai_transport_server_admit(v->srv, fy_get(m, "id", 0LL),
					 fy_get(m, "parent", 0LL),
					 (pid_t)fy_get(m, "pid", 0LL),
					 (uid_t)fy_get(m, "uid", -1LL), fd,
					 allow, n, &ns);
	v->fd_taken = !rc;
	return rc;
}

static int op_grant(struct fyai_transport_verb *v, struct fy_generic_builder *gb,
		    fy_generic m)
{
	struct fyai_transport_allow allow[FYAI_CTL_MAX_GRANT];
	size_t n;
	int rc;

	rc = fyai_ctl_grant_parse(gb, fy_get(m, "grant", fy_invalid), allow,
				  FYAI_CTL_MAX_GRANT, &n);
	if (rc)
		return rc;
	rc = fyai_transport_server_set_grant(v->srv, fy_get(m, "id", 0LL), allow, n);
	return rc;
}

static int op_profiles(struct fyai_transport_verb *v, fy_generic m)
{
	struct fyai_transport_grant grant = { 0 };
	const char *why = "the profiles are not valid";
	int rc;

	rc = fyai_ctl_profiles_parse(fy_get(m, "profiles", fy_invalid), &grant, &why);
	if (rc)
		return rc;
	rc = fyai_transport_server_set_profiles(v->srv, &grant);
	fyai_transport_grant_clear(&grant);
	return rc;
}

static int verb_dispatch(struct fyai_transport_verb *v, struct fy_generic_builder *gb,
			 fy_generic m, int fd, const char **why)
{
	fy_generic opv = fy_get(m, "op", fy_invalid);
	fy_generic name, value;
	const char *op = fy_castp(&opv, "");
	int rc = 0;

	if (!strcmp(op, "init"))
		rc = op_init(v, m);
	else if (!strcmp(op, "credential")) {
		name = fy_get(m, "name", fy_invalid);
		value = fy_get(m, "value", fy_invalid);
		if (!fy_is_string(name) || fy_empty(name) ||
		    !fy_is_string(value) || fy_empty(value)) {
			*why = "a credential needs a name and a value";
			rc = -EINVAL;
		} else
			rc = mem_set(v, fy_castp(&name, ""), fy_castp(&value, ""));
	} else if (!strcmp(op, "shutdown"))
		v->done = true;
	else if (!v->inited) {
		*why = "init comes first";
		rc = -EINVAL;
	} else if (!strcmp(op, "profiles"))
		rc = op_profiles(v, m);
	else if (!strcmp(op, "admit"))
		rc = op_admit(v, gb, m, fd);
	else if (!strcmp(op, "grant"))
		rc = op_grant(v, gb, m);
	else if (!strcmp(op, "retire"))
		rc = fyai_transport_server_retire(v->srv, fy_get(m, "id", 0LL));
	else if (!strcmp(op, "log")) {
		v->ctx->cfg->transport_logging = fy_get(m, "on", false);
		v->ctx->cfg->wire_logging = fy_get(m, "wire", false);
		v->ctx->cfg->whitewash_api_keys = fy_get(m, "whitewash", true);
	}
	else {
		*why = "unknown op";
		rc = -EINVAL;
	}
	if (rc && !*why)
		*why = errno_text(rc);
	return rc;
}

static int verb_message(struct fyai_transport_verb *v)
{
	char storage[CTL_BUILDER_SIZE];
	struct fy_generic_builder *gb;
	fy_generic m, reply;
	const char *why = NULL;
	int fd = -1, rc;

	gb = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
						 storage, sizeof(storage));
	if (!gb)
		return -ENOMEM;
	rc = fyai_ctl_recv(v->ctl, gb, &m, &fd);
	if (rc)
		return rc;
	v->fd_taken = false;
	rc = verb_dispatch(v, gb, m, fd, &why);
	/* Only an admitted channel outlives its request. */
	if (fd >= 0 && !v->fd_taken)
		close(fd);
	reply = rc ? fyai_ctl_reply_error(gb, fy_get(m, "seq", 0LL), why) :
		      fyai_ctl_reply_ok(gb, fy_get(m, "seq", 0LL));
	return fyai_ctl_send(v->ctl, reply, -1, 0);
}

static enum fyai_event_action ctl_on_event(const struct fyai_event *ev)
{
	struct fyai_transport_verb *v = ev->userdata;
	unsigned int n;
	int rc;

	if (ev->events & FYAIEV_ERROR) {
		v->done = true;
		return FYAIEA_STOP;
	}
	/* Bound the work of one wake so the channels are served in turn. */
	for (n = 0; n < CTL_WAKE_BATCH && !v->done; n++) {
		rc = verb_message(v);
		if (rc == -EAGAIN)
			break;
		if (rc)
			v->done = true;
	}
	return v->done ? FYAIEA_STOP : FYAIEA_CONTINUE;
}

static void verb_cleanup(struct fyai_transport_verb *v)
{
	size_t i;

	fyai_transport_server_destroy(v->srv);
	fyai_transport_registry_destroy(v->reg);
	for (i = 0; i < v->nmem; i++) {
		fyai_secret_clear(v->mem[i].value, strlen(v->mem[i].value));
		free(v->mem[i].value);
		free(v->mem[i].name);
	}
	free(v->mem);
}

static int fyai_transport_verb(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;
	struct fyai_transport_verb v = { .ctx = ctx, .ctl = ctx->cfg->transport_ctl_fd };
	int rc;

	fyai_error_check(ctx, v.ctl >= 0, err,
			 "transport: there is no control channel");
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err, "transport: cannot create the event loop");
	rc = fyai_event_add_fd(el, v.ctl, FYAIEV_READ, ctl_on_event, &v, &v.src);
	fyai_error_check(ctx, !rc, err,
			 "transport: cannot watch the control channel");

	/* Serve until the channel closes, or the supervisor says shutdown. */
	rc = fyai_event_loop_run_until(el, &v.done, -1);
	if (v.src)
		fyai_event_source_remove(v.src);
	verb_cleanup(&v);
	return rc < 0 ? -1 : 0;
err:
	return -1;
}

int fyai_cmd_transport(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_transport_verb(call->ctx);
}
