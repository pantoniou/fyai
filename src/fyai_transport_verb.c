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
#include "fyai_auth.h"
#include "fyai_cmd_int.h"
#include "fyai_event.h"
#include "fyai_secret.h"
#include "fyai_transport.h"
#include "fyai_transport_ctl.h"
#include "fyai_transport_sock.h"
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

struct fyai_transport_verb;

/*
 * A control connection. The first one, which starts the transport, is the
 * primary: its close or a shutdown request ends the transport, and it gets the
 * events. Every agent process gets a connection of its own, so that it can
 * admit its children; the end of one of those only
 * removes it. It acts for its own execution and the descendants of it, and
 * for no other.
 */
struct fyai_transport_conn {
	struct fyai_transport_conn *next;
	struct fyai_transport_verb *v;
	int fd;
	struct fyai_event_source *src;
	bool primary;
	bool dead;
	uint64_t exec;		/* a secondary connection: the execution it belongs to */
};

struct fyai_transport_verb {
	struct fyai_ctx *ctx;
	struct fyai_transport_conn *conns;
	struct fyai_transport_conn *dead;		/* removed by a deferred call */
	int ctl;			/* the primary connection */
	struct fyai_transport_registry *reg;
	struct fyai_transport_server *srv;
	struct mem_cred *mem;
	size_t nmem;
	struct fyai_auth_refresh_request *refresh;	/* the login being refreshed */
	uint64_t next_id;		/* execution ids assigned to admissions */
	long long reply_id;		/* 0, or an id to report in the reply */
	fy_generic reply_extra;		/* fields merged into the reply, or invalid */
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
static int cred_env(struct fyai_transport_verb *v, const char *name, char **secret,
		    struct curl_slist **extra)
{
	const char *value;

	(void)v;
	(void)extra;
	value = getenv(name);
	if (!value || !*value)
		return -ENOENT;
	*secret = strdup(value);
	return *secret ? 0 : -ENOMEM;
}

static int cred_mem(struct fyai_transport_verb *v, const char *name, char **secret,
		    struct curl_slist **extra)
{
	const struct mem_cred *m;

	(void)extra;
	m = mem_find(v, name);
	if (!m || !m->value || !*m->value)
		return -ENOENT;
	*secret = strdup(m->value);
	return *secret ? 0 : -ENOMEM;
}

static int cred_secret(struct fyai_transport_verb *v, const char *name, char **secret,
		       struct curl_slist **extra)
{
	char key[300], *found = NULL;
	size_t flen = 0;
	int rc, n;

	(void)v;
	(void)extra;
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

/*
 * "oauth:chatgpt": the access token of the subscription login, which only this
 * process reads. A token that cannot be used is reported, not refreshed here:
 * cred_prepare() does that before a request reads it.
 */
static int cred_oauth(struct fyai_transport_verb *v, const char *name, char **secret,
		      struct curl_slist **extra)
{
	(void)extra;
	if (strcmp(name, "chatgpt"))
		return -EINVAL;
	if (fyai_auth_store_load(v->ctx))
		return -ENOENT;
	if (!fyai_auth_store_ready(v->ctx))
		return -ENOENT;
	if (!fyai_auth_store_valid(v->ctx))
		return -ESTALE;
	*secret = strdup(fyai_auth_store_token(v->ctx));
	if (!*secret)
		return -ENOMEM;
	return 0;
}

struct credential_source {
	const char *prefix;
	int (*get)(struct fyai_transport_verb *, const char *, char **,
		   struct curl_slist **);
};

static const struct credential_source credential_sources[] = {
	{ "env:", cred_env },
	{ "mem:", cred_mem },
	{ "secret:", cred_secret },
	{ "oauth:", cred_oauth },
};

static int cred_one(struct fyai_transport_verb *v, const char *src, size_t len, char **secret,
		    struct curl_slist **extra)
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
			return credential_sources[i].get(v, tmp + n, secret, extra);
	}
	return -EINVAL;
}

/*
 * Resolve the credential of a profile. The source can be a chain, "A|B": the
 * first source that holds a value wins, as the provider default tries the
 * environment and then the secret store.
 */
static int verb_cred(void *ud, const struct fyai_transport_profile *pr,
		     char **secret, struct curl_slist **extra)
{
	const char *src = pr->credential;
	const char *end;
	size_t len;
	int rc = -ENOENT;

	*secret = NULL;
	while (src && *src) {
		end = strchr(src, '|');
		len = end ? (size_t)(end - src) : strlen(src);

		rc = cred_one(ud, src, len, secret, extra);
		if (!rc || rc == -ENOMEM)
			return rc;
		src = end ? end + 1 : NULL;
	}
	return rc == -EINVAL || rc == -ESTALE ? rc : -ENOENT;
}

static void refresh_done(struct fyai_auth_refresh_request *request, void *ud)
{
	struct fyai_transport_verb *v = ud;

	(void)fyai_auth_refresh_collect(request);
	fyai_auth_refresh_destroy(request);
	v->refresh = NULL;
	fyai_transport_server_prepared(v->srv);
}

/*
 * Refresh the subscription login before a request uses it. One refresh serves
 * every request that waits: the first starts it and the others park. A request
 * that waited never starts another, so a login that cannot be refreshed ends
 * each waiting request with an error instead of starting a refresh each time.
 */
static int verb_prepare(void *ud, const struct fyai_transport_profile *pr,
			bool resumed)
{
	struct fyai_transport_verb *v = ud;

	if (!pr->credential || strcmp(pr->credential, "oauth:chatgpt"))
		return 0;
	if (fyai_auth_store_load(v->ctx))
		return -ENOENT;
	if (fyai_auth_store_fresh(v->ctx))
		return 0;
	if (resumed)
		return fyai_auth_store_valid(v->ctx) ? 0 : -ESTALE;
	if (v->refresh)
		return 1;
	v->refresh = fyai_auth_refresh_submit(v->ctx, false, refresh_done, v);
	if (!v->refresh)
		return fyai_auth_store_valid(v->ctx) ? 0 : -EIO;
	/* A login that needs no refresh ends at once, with no callback. */
	if (fyai_auth_refresh_done(v->refresh)) {
		fyai_auth_refresh_destroy(v->refresh);
		v->refresh = NULL;
		(void)fyai_auth_store_load(v->ctx);
		return fyai_auth_store_valid(v->ctx) ? 0 : -ESTALE;
	}
	return 1;
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
	fyai_transport_server_set_prepare(srv, verb_prepare);
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

static enum fyai_event_action conn_on_event(const struct fyai_event *ev);

/* What the transport enforces, from the transport itself. */
static int op_status(struct fyai_transport_verb *v, struct fy_generic_builder *gb)
{
	fy_generic execs = fy_seq_empty, profiles = fy_seq_empty;
	size_t i, n;
	uint64_t id, parent;
	pid_t pid;

	n = fyai_transport_registry_count(v->reg);
	for (i = 0; i < n; i++) {
		if (!fyai_transport_registry_exec_info(v->reg, i, &id, &parent, &pid))
			break;
		execs = fy_append(gb, execs,
				  fy_mapping(gb, "id", (long long)id,
					     "parent", (long long)parent,
					     "pid", (long long)pid));
	}
	n = fyai_transport_server_profile_count(v->srv);
	for (i = 0; i < n; i++)
		profiles = fy_append(gb, profiles,
				     fy_value(gb, fyai_transport_server_profile_name(v->srv, i)));
	v->reply_extra = fy_mapping(gb,
		"level", fyai_transport_level_name(fyai_transport_registry_level(v->reg)),
		"pid", (long long)getpid(),
		"executions", execs, "profiles", profiles,
		"active", (long long)fyai_transport_server_active(v->srv),
		"log", v->ctx->cfg->transport_logging,
		"wire", v->ctx->cfg->wire_logging);
	return 0;
}

/* Does a credential source hold a value? The answer is yes or no, never the value. */
static int op_probe(struct fyai_transport_verb *v, struct fy_generic_builder *gb,
		    fy_generic m, const char **detail)
{
	fy_generic src = fy_get(m, "credential", fy_invalid);
	struct fyai_transport_profile pr = { 0 };
	struct curl_slist *extra = NULL;
	char *secret = NULL;
	int rc;

	if (!fy_is_string(src) || fy_empty(src)) {
		*detail = "probe needs a credential source";
		return -EINVAL;
	}
	pr.credential = (char *)fy_castp(&src, "");
	rc = verb_cred(v, &pr, &secret, &extra);
	if (secret) {
		fyai_secret_clear(secret, strlen(secret));
		free(secret);
	}
	curl_slist_free_all(extra);
	/* A comparison is an int in C; the reply needs a JSON boolean. */
	v->reply_extra = fy_mapping(gb, "found", (bool)(rc == 0));
	return 0;
}

/*
 * Send the value of the named variables into the socket that the request
 * carries, for one command that the user configured, such as the catalogue
 * scraper. The requester never reads the socket: the command child does. Only
 * the primary connection may ask: an agent cannot.
 */
static int op_envgrant(struct fyai_transport_conn *c, fy_generic m, int fd,
		       const char **detail)
{
	fy_generic names = fy_get(m, "names", fy_invalid), name;
	char buf[65536];
	size_t n = 0, len = 0;
	ssize_t sent;
	const char *k, *val;
	int w, rc;

	if (!c->primary) {
		*detail = "only the primary connection may ask for a credential grant";
		return -EPERM;
	}
	if (fd < 0) {
		*detail = "envgrant needs the socket descriptor";
		return -EBADF;
	}
	if (!fy_is_sequence(names)) {
		*detail = "envgrant needs a list of names";
		return -EINVAL;
	}
	fy_foreach(name, names) {
		k = fy_is_string(name) ? fy_castp(&name, "") : NULL;
		val = k && *k ? getenv(k) : NULL;

		if (++n > 16) {
			*detail = "too many names";
			rc = -E2BIG;
			goto out;
		}
		if (!val || !*val)
			continue;
		w = snprintf(buf + len, sizeof(buf) - len, "%s=%s", k, val);
		if (w < 0 || (size_t)w + 1 >= sizeof(buf) - len) {
			*detail = "the credentials are too large";
			rc = -E2BIG;
			goto out;
		}
		len += (size_t)w + 1;
	}

	sent = send(fd, buf, len, MSG_NOSIGNAL);
	rc = sent < 0 ? -errno : (size_t)sent != len ? -EIO : 0;
	if (rc)
		*detail = "cannot send the credentials";
out:
	fyai_secret_clear(buf, sizeof(buf));
	return rc;
}

/* Is @id registered, and if so, whose child is it? */
static bool exec_parent(struct fyai_transport_verb *v, uint64_t id, uint64_t *parent)
{
	size_t i, n = fyai_transport_registry_count(v->reg);
	uint64_t eid, p;
	pid_t pid;

	for (i = 0; i < n; i++) {
		if (!fyai_transport_registry_exec_info(v->reg, i, &eid, &p, &pid))
			break;
		if (eid == id) {
			*parent = p;
			return true;
		}
	}
	return false;
}

/*
 * May @c act for execution @id? The primary may act for every one. A
 * secondary connection may act for its own execution, when @self_ok, and for
 * the descendants of it.
 */
static bool conn_owns(struct fyai_transport_conn *c, uint64_t id, bool self_ok)
{
	uint64_t cur = id, parent;
	unsigned int depth;

	if (c->primary)
		return true;
	for (depth = 0; depth < 64; depth++) {
		if (cur == c->exec)
			return self_ok || cur != id;
		if (!cur || !exec_parent(c->v, cur, &parent))
			return false;
		cur = parent;
	}
	return false;
}

/* The transport owns @fd on success. */
static int op_ctl(struct fyai_transport_conn *sender, fy_generic m, int fd,
		  const char **detail)
{
	struct fyai_transport_verb *v = sender->v;
	uint64_t id = fy_get(m, "id", 0LL), parent;
	struct fyai_transport_conn *c;
	int rc;

	if (fd < 0) {
		*detail = "ctl needs the channel descriptor";
		return -EBADF;
	}
	if (!id || !exec_parent(v, id, &parent)) {
		*detail = "ctl names no admitted execution";
		return -ENOENT;
	}
	if (!conn_owns(sender, id, false)) {
		*detail = "the execution is not a descendant of the caller";
		return -EPERM;
	}
	c = calloc(1, sizeof(*c));
	if (!c) {
		*detail = "cannot allocate the control connection";
		return -ENOMEM;
	}
	c->v = v;
	c->fd = fd;
	c->exec = id;
	rc = fyai_event_add_fd(fyai_ctx_loop(v->ctx), fd, FYAIEV_READ,
			       conn_on_event, c, &c->src);
	if (rc) {
		free(c);
		*detail = "cannot watch the control channel";
		return rc;
	}
	c->next = v->conns;
	v->conns = c;
	v->fd_taken = true;
	return 0;
}

static bool pool_has(struct fyai_transport_verb *v, const char *name)
{
	size_t i, n = fyai_transport_server_profile_count(v->srv);

	for (i = 0; i < n; i++)
		if (!strcmp(fyai_transport_server_profile_name(v->srv, i), name))
			return true;
	return false;
}

/*
 * A grant names profiles of the set that the user session stated, and no
 * other. A connection that is not the primary grants no more than its own
 * execution holds.
 */
static int grant_check(struct fyai_transport_conn *c, const struct fyai_transport_allow *allow,
			       size_t n, const char **detail)
{
	struct fyai_transport_exec *own = NULL;
	const char *model;
	size_t i;

	if (!c->primary)
		own = fyai_transport_find(c->v->reg, c->exec);
	for (i = 0; i < n; i++) {
		if (!pool_has(c->v, allow[i].profile)) {
			*detail = "the grant names a profile that the set does not have";
			return -ENOENT;
		}
		if (!c->primary &&
		    (!own || !fyai_transport_exec_allows(own, allow[i].profile, &model))) {
			*detail = "the grant names a profile that the caller does not hold";
			return -EPERM;
		}
	}
	return 0;
}

static int op_admit(struct fyai_transport_conn *c, struct fy_generic_builder *gb,
		    fy_generic m, int fd, const char **detail)
{
	struct fyai_transport_verb *v = c->v;
	struct fyai_transport_allow allow[FYAI_CTL_MAX_GRANT];
	struct fyai_transport_ns_req ns;
	long long id = fy_get(m, "id", 0LL);
	size_t n;
	int rc;

	if (fd < 0)
		return -EBADF;
	if (!conn_owns(c, fy_get(m, "parent", 0LL), true)) {
		*detail = "the parent is not the caller or a descendant of it";
		return -EPERM;
	}
	rc = fyai_ctl_grant_parse(gb, fy_get(m, "grant", fy_invalid), allow,
				  FYAI_CTL_MAX_GRANT, &n);
	if (rc)
		return rc;
	rc = fyai_ctl_ns_parse(fy_get(m, "ns", fy_invalid), &ns);
	if (rc)
		return -EINVAL;
	rc = grant_check(c, allow, n, detail);
	if (rc)
		return rc;
	/* An admission with no id gets the next one, and the reply says which. */
	if (id <= 0)
		id = v->next_id++;
	rc = fyai_transport_server_admit(v->srv, id, fy_get(m, "parent", 0LL),
					 (pid_t)fy_get(m, "pid", 0LL),
					 (uid_t)fy_get(m, "uid", -1LL), fd,
					 allow, n, &ns);
	v->fd_taken = !rc;
	v->reply_id = rc ? 0 : id;
	return rc;
}

static int op_grant(struct fyai_transport_conn *c, struct fy_generic_builder *gb,
		    fy_generic m, const char **detail)
{
	struct fyai_transport_verb *v = c->v;
	struct fyai_transport_allow allow[FYAI_CTL_MAX_GRANT];
	size_t n;
	int rc;

	/* The parent gives a grant; an agent does not widen its own. */
	if (!conn_owns(c, fy_get(m, "id", 0LL), false)) {
		*detail = "the execution is not a descendant of the caller";
		return -EPERM;
	}
	rc = fyai_ctl_grant_parse(gb, fy_get(m, "grant", fy_invalid), allow,
				  FYAI_CTL_MAX_GRANT, &n);
	if (rc)
		return rc;
	rc = grant_check(c, allow, n, detail);
	if (rc)
		return rc;
	rc = fyai_transport_server_set_grant(v->srv, fy_get(m, "id", 0LL), allow, n);
	return rc;
}

/*
 * "profiles" replaces the set; with "merge" it adds to it, as the user session
 * does when its provider or model changes. Only the primary connection, which
 * is the user session, changes the set.
 */
static int op_profiles(struct fyai_transport_conn *c, fy_generic m)
{
	struct fyai_transport_verb *v = c->v;
	struct fyai_transport_grant grant = { 0 };
	const char *why = "the profiles are not valid";
	int rc;

	if (!c->primary)
		return -EPERM;
	rc = fyai_ctl_profiles_parse(fy_get(m, "profiles", fy_invalid), &grant, &why);
	if (rc)
		return rc;
	if (fy_get(m, "merge", false))
		rc = fyai_transport_server_add_profiles(v->srv, &grant);
	else
		rc = fyai_transport_server_set_profiles(v->srv, &grant);
	fyai_transport_grant_clear(&grant);
	return rc;
}

static int verb_dispatch(struct fyai_transport_conn *c, struct fy_generic_builder *gb,
			 fy_generic m, int fd, const char **why)
{
	struct fyai_transport_verb *v = c->v;
	fy_generic opv = fy_get(m, "op", fy_invalid);
	fy_generic name, value;
	const char *op = fy_castp(&opv, "");
	const char *detail = NULL;
	int rc = 0;

	if (!strcmp(op, "init")) {
		if (!c->primary) {
			detail = "only the primary connection starts the transport";
			rc = -EPERM;
		} else
			rc = op_init(v, m);
		goto out;
	}
	if (!strcmp(op, "credential")) {
		name = fy_get(m, "name", fy_invalid);
		value = fy_get(m, "value", fy_invalid);
		if (!c->primary) {
			detail = "only the primary connection sets a credential";
			rc = -EPERM;
		} else if (!fy_is_string(name) || fy_empty(name) ||
			   !fy_is_string(value) || fy_empty(value)) {
			detail = "a credential needs a name and a value";
			rc = -EINVAL;
		} else
			rc = mem_set(v, fy_castp(&name, ""), fy_castp(&value, ""));
		goto out;
	}
	if (!strcmp(op, "shutdown")) {
		if (!c->primary) {
			detail = "only the primary connection ends the transport";
			rc = -EPERM;
		} else
			v->done = true;
		goto out;
	}
	if (!v->inited) {
		detail = "init comes first";
		rc = -EINVAL;
		goto out;
	}
	if (!strcmp(op, "ctl"))
		rc = op_ctl(c, m, fd, &detail);
	else if (!strcmp(op, "status"))
		rc = op_status(v, gb);
	else if (!strcmp(op, "probe"))
		rc = op_probe(v, gb, m, &detail);
	else if (!strcmp(op, "envgrant"))
		rc = op_envgrant(c, m, fd, &detail);
	else if (!strcmp(op, "profiles")) {
		rc = op_profiles(c, m);
		if (rc == -EPERM)
			detail = "only the primary connection changes the profiles";
	} else if (!strcmp(op, "admit")) {
		rc = op_admit(c, gb, m, fd, &detail);
	} else if (!strcmp(op, "grant")) {
		rc = op_grant(c, gb, m, &detail);
	} else if (!strcmp(op, "retire")) {
		if (!conn_owns(c, fy_get(m, "id", 0LL), true)) {
			detail = "the execution is not the caller or a descendant of it";
			rc = -EPERM;
		} else
			rc = fyai_transport_server_retire(v->srv, fy_get(m, "id", 0LL));
	} else if (!strcmp(op, "log")) {
		if (!c->primary) {
			detail = "only the primary connection changes the logs";
			rc = -EPERM;
		} else {
			v->ctx->cfg->transport_logging = fy_get(m, "on", false);
			v->ctx->cfg->wire_logging = fy_get(m, "wire", false);
			v->ctx->cfg->whitewash_api_keys = fy_get(m, "whitewash", true);
		}
	} else {
		detail = "unknown op";
		rc = -EINVAL;
	}
out:
	*why = rc ? detail ? detail : errno_text(rc) : NULL;
	return rc;
}

static void conn_reap(void *userdata)
{
	struct fyai_transport_verb *v = userdata;
	struct fyai_transport_conn *c;

	while ((c = v->dead)) {
		v->dead = c->next;
		if (c->src)
			fyai_event_source_remove(c->src);
		close(c->fd);
		free(c);
	}
}

/* Take a connection out of service; a deferred call frees it. */
static void conn_drop(struct fyai_transport_conn *c)
{
	struct fyai_transport_verb *v = c->v;
	struct fyai_transport_conn **pp;

	if (c->dead)
		return;
	c->dead = true;
	for (pp = &v->conns; *pp; pp = &(*pp)->next) {
		if (*pp == c) {
			*pp = c->next;
			break;
		}
	}
	c->next = v->dead;
	v->dead = c;
	fyai_event_defer(fyai_ctx_loop(v->ctx), conn_reap, v);
}

static int verb_message(struct fyai_transport_conn *c)
{
	char storage[CTL_BUILDER_SIZE];
	struct fy_generic_builder *gb;
	struct fyai_transport_verb *v = c->v;
	fy_generic m, reply;
	fy_generic k, val;
	const char *why;
	int fd = -1, rc;

	gb = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
						 storage, sizeof(storage));
	if (!gb)
		return -ENOMEM;
	rc = fyai_ctl_recv(c->fd, gb, &m, &fd);
	if (rc)
		return rc;
	v->fd_taken = false;
	v->reply_id = 0;
	v->reply_extra = fy_invalid;
	rc = verb_dispatch(c, gb, m, fd, &why);
	/* Only an admitted or attached channel outlives its request. */
	if (fd >= 0 && !v->fd_taken)
		close(fd);
	reply = rc ? fyai_ctl_reply_error(gb, fy_get(m, "seq", 0LL), why) :
		      fyai_ctl_reply_ok(gb, fy_get(m, "seq", 0LL));
	if (!rc && v->reply_id)
		reply = fy_assoc(gb, reply, fy_value(gb, "id"),
				 fy_value(gb, v->reply_id));
	if (!rc && fy_is_mapping(v->reply_extra)) {
		fy_foreach_key_value(k, val, v->reply_extra)
			reply = fy_assoc(gb, reply, k, val);
	}
	return fyai_ctl_send(c->fd, reply, -1, 0);
}

static enum fyai_event_action conn_on_event(const struct fyai_event *ev)
{
	struct fyai_transport_conn *c = ev->userdata;
	struct fyai_transport_verb *v = c->v;
	unsigned int n;
	int rc;

	if (c->dead)
		return FYAIEA_CONTINUE;
	if (ev->events & FYAIEV_ERROR) {
		if (c->primary)
			v->done = true;
		else
			conn_drop(c);
		return v->done ? FYAIEA_STOP : FYAIEA_CONTINUE;
	}
	/* Bound the work of one wake so the channels are served in turn. */
	for (n = 0; n < CTL_WAKE_BATCH && !v->done && !c->dead; n++) {
		rc = verb_message(c);
		if (rc == -EAGAIN)
			break;
		if (rc) {
			if (c->primary)
				v->done = true;
			else
				conn_drop(c);
		}
	}
	return v->done ? FYAIEA_STOP : FYAIEA_CONTINUE;
}

static void verb_cleanup(struct fyai_transport_verb *v)
{
	struct fyai_transport_conn *c;
	size_t i;

	while ((c = v->conns)) {
		v->conns = c->next;
		if (c->src)
			fyai_event_source_remove(c->src);
		if (!c->primary)
			close(c->fd);
		free(c);
	}
	conn_reap(v);
	if (v->refresh) {
		fyai_auth_refresh_cancel(v->refresh);
		fyai_auth_refresh_destroy(v->refresh);
		v->refresh = NULL;
	}
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
	struct fyai_transport_verb v = { .ctx = ctx, .ctl = ctx->cfg->transport_ctl_fd,
			  .next_id = 2 };
	struct fyai_transport_conn *c;
	int rc;

	sigset_t term;

	fyai_error_check(ctx, v.ctl >= 0, err,
			 "transport: there is no control channel");
	/*
	 * The transport has no signal event source. Unblock SIGTERM and SIGHUP
	 * so that their default actions terminate the process.
	 */
	sigemptyset(&term);
	sigaddset(&term, SIGTERM);
	sigaddset(&term, SIGHUP);
	sigprocmask(SIG_UNBLOCK, &term, NULL);
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err, "transport: cannot create the event loop");
	c = calloc(1, sizeof(*c));
	fyai_error_check(ctx, c, err, "transport: out of memory");
	c->v = &v;
	c->fd = v.ctl;
	c->primary = true;
	v.conns = c;
	rc = fyai_event_add_fd(el, v.ctl, FYAIEV_READ, conn_on_event, c, &c->src);
	fyai_error_check(ctx, !rc, err_conn,
			 "transport: cannot watch the control channel");

	/* Serve until the primary channel closes, or the supervisor says shutdown. */
	rc = fyai_event_loop_run_until(el, &v.done, -1);
	verb_cleanup(&v);
	return rc < 0 ? -1 : 0;
err_conn:
	v.conns = NULL;
	free(c);
err:
	return -1;
}

int fyai_cmd_transport(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_transport_verb(call->ctx);
}
