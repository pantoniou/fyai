/*
 * fyai_shell_sandbox.c - shell sandbox
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef __FILC__
#include <stdfil.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <termios.h>
#include "fyai_reqcache.h"
#include "fyai_host.h"
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "fyai_transport_boot.h"
#include "fyai_transport_sock.h"
#include "fyai_agent.h"
#include "fyai_monitor.h"
#include "fyai_tool_registry.h"
#include "fyai_branch.h"
#include "fyai_browser.h"
#include "fyai_agents.h"
#include "fyai_chrome.h"
#include "fyai_jsonrpc.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_markdown.h"
#include "fyai_patch.h"
#include "fyai_sandbox.h"
#include "fyai_sandbox_profile.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
#include "fyai_terminal_session.h"
#include "fyai_output.h"
#include "fyai_ask.h"
#include "fyai_skill.h"
#include "fyai_settings.h"
#include "fyai_todo.h"
#include "fyai_home.h"
#include "fyai_tools.h"
#include "fyai_tools_int.h"
#include "fyai_orphan.h"
#include "fyai_view.h"
#include "fyai_cover.h"
#include "fyai_fsview.h"
#include "fyai_asks.h"
#include "fyai_wait.h"
#include "fyai_prof.h"
#include "fyai_render.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_uimodel.h"
#include "fyai_uilog.h"
#include "fyai_uiwork.h"
#include "fyai_uitranscript.h"
#include "fyai_workpane.h"

/*
 * Add an entry to the allow list of @sb, which grows as it needs. The list owns
 * @path afterwards, and frees it when the add fails. A path that the list holds
 * already is added again: the kernel merges the rules of one path.
 */
static int fyai_shell_sandbox_allow(struct fyai_ctx *ctx, struct fyai_shell_sandbox *sb,
				    char *path, enum fyai_sandbox_mode mode)
{
	struct fyai_sandbox_path *grown;
	size_t cap;

	if (sb->spec.allow_n == sb->allow_cap) {
		cap = sb->allow_cap ? sb->allow_cap * 2 : 8;
		grown = realloc(sb->allow, cap * sizeof(*sb->allow));
		if (!grown) {
			free(path);
			fyai_error(ctx, "sandbox: could not grow the allow list");
			return -1;
		}
		sb->allow = grown;
		sb->allow_cap = cap;
	}
	sb->allow[sb->spec.allow_n].path = path;
	sb->allow[sb->spec.allow_n].mode = mode;
	sb->spec.allow_n++;
	return 0;
}

/*
 * Resolve a config path against @base: absolute as-is, "~"-prefixed against
 * $HOME, otherwise relative to @base (the project root). Returns a malloc'd
 * absolute path or NULL.
 */
char *fyai_sandbox_resolve_path(const char *base, const char *p)
{
	const char *home;
	char *out;

	if (!p || !*p)
		return NULL;
	if (p[0] == '/')
		return strdup(p);
	if (p[0] == '~') {
		home = getenv("HOME");
		if (!home || asprintf(&out, "%s%s", home, p + 1) < 0)
			return NULL;
		return out;
	}
	if (!base)
		return strdup(p);
	return asprintf(&out, "%s/%s", base, p) < 0 ? NULL : out;
}

/*
 * Build the tool sandbox spec from cfg->sandbox, or return NULL when disabled.
 * Confinement is scoped to the project root (or cwd when none is found), with
 * the arena .fyai plus every config deny entry carved out, the config allow
 * entries added, and the configured TCP and UDP network policies applied.
 */
int fyai_shell_sandbox_build(struct fyai_ctx *ctx,
			     struct fyai_shell_sandbox *sb,
			     const struct fyai_sandbox_spec **specp)
{
	struct fyai_sandbox_spec *sp = &sb->spec;
	fy_generic cs = ctx->cfg->sandbox;
	fy_generic allow, deny, net, tcp, udp, ports, e;
	fy_generic port, pv;
	enum fyai_sandbox_mode mode;
	char **cand = NULL;
	enum fyai_cover_method method;
	enum fyai_cover_start start;
	pid_t holder = 0;
	size_t cn = 0, gn, i, mounts;
	char cwd[4096];
	char *resolved;
	const char *ps;
	size_t n;
	int rc;

	memset(sb, 0, sizeof(*sb));
	*specp = NULL;
	if (!ctx->cfg->enable_sandbox || ctx->sandbox_applied)
		return 0;

	sb->root = ctx->cfg->view_project ? strdup(ctx->cfg->view_project) :
		   fyai_discover_project_root();
	if (!sb->root && getcwd(cwd, sizeof(cwd)))
		sb->root = strdup(cwd);
	fyai_error_check(ctx, sb->root, err_out,
			 "sandbox: could not resolve the project root");
	sp->project_root = sb->root;
	sp->scratch_root = ctx->cfg->view_scratch;
	sp->strict = false;			/* floor is the config policy */
	sp->filter_path = true;

	/* The mounts of the profile need the namespaces that the cover makes. */
	if (!ctx->cfg->tool_child && fyai_home_ensure(ctx))
		goto err_out;
	/*
	 * deny: each config sandbox.deny entry, then the arena. Where a cover is
	 * possible (fyai_cover.h), each existing path is hidden in a mount namespace
	 * and Landlock gets no deny: a denied path under a granted directory would
	 * leave that directory only the right to list, so a tool could not create
	 * entries in the project root. Elsewhere Landlock denies the path.
	 */
	deny = fy_get(cs, "deny");
	n = fy_is_sequence(deny) ? fy_len(deny) : 0;
	sb->deny = calloc(n + 2, sizeof(*sb->deny));
	sb->hide = calloc(n + 2, sizeof(*sb->hide));
	cand = calloc(n + 2, sizeof(*cand));
	fyai_error_check(ctx, sb->deny && sb->hide && cand, err_out,
			 "sandbox: could not allocate the deny list");
	/* Configured denies apply to every grant. Ignore paths that do not exist. */
	fy_foreach(e, deny) {
		ps = fy_castp(&e, "");
		resolved = fyai_sandbox_resolve_path(sb->root, ps);
		fyai_error_check(ctx, resolved, err_out,
				 "sandbox: could not resolve deny path '%s'", ps);
		if (access(resolved, F_OK)) {
			free(resolved);
			continue;
		}
		cand[cn++] = resolved;
	}
	gn = cn;
	resolved = fyai_sandbox_resolve_path(sb->root, ".fyai");
	fyai_error_check(ctx, resolved, err_out,
			 "sandbox: could not resolve the arena deny path");
	if (ctx->cfg->view_project || access(resolved, F_OK))
		free(resolved);
	else
		cand[cn++] = resolved;
	/* A session in a view keeps the carve-out: the view hides the arena already. */
	/*
	 * The invocation starts the holder of the scope of the exchange, and each tool
	 * child joins it. A scope of one call is made by the tool itself.
	 */
	if (ctx->cfg->sandbox_scope_kind == FYAI_SCOPE_CALL)
		start = FYAI_COVER_START_CALL;
	else
		start = ctx->cfg->tool_child ? FYAI_COVER_JOIN_ONLY :
					       FYAI_COVER_START_SCOPE;
	fyai_fsview_set_grace((long)ctx->cfg->sandbox_grace_ms);
	method = ctx->cfg->view_project ? FYAI_COVER_NONE :
		 fyai_cover_select(&ctx->cover, ctx->cfg->sandbox_cover,
				   (const char *const *)cand, cn, &holder, start);
	for (i = 0; i < cn; i++) {
		if (method == FYAI_COVER_USERNS ||
		    (method == FYAI_COVER_BWRAP && fyai_cover_covers(&ctx->cover, cand[i]))) {
			sb->hide[sb->hide_n++] = cand[i];
		} else {
			sb->deny[sp->deny_n++] = cand[i];
			if (i < gn)
				sp->deny_global_n++;
		}
	}
	free(cand);
	cand = NULL;
	sp->deny = sb->deny;
	sp->hide = (const char *const *)sb->hide;
	sp->hide_n = sb->hide_n;
	mounts = ctx->home ? ctx->home->mount_n : 0;
	fyai_error_check(ctx, !mounts || method != FYAI_COVER_NONE, err_out,
			 ctx->cfg->view_project ?
			 "sandbox: the profile makes mounts, which a view does not support yet" :
			 "sandbox: the profile makes mounts, which need a user namespace; "
			 "set sandbox_cover to auto and allow user namespaces (see "
			 "doc/apparmor-userns.md)");
	if (method == FYAI_COVER_BWRAP && (sb->hide_n || mounts))
		sp->cover_holder = holder;

	/* allow: extra grants; a string is rw, a mapping {path, mode: ro}. */
	allow = fy_get(cs, "allow");
	fy_foreach(e, allow) {
		mode = FYAI_SB_RW;
		if (fy_is_mapping(e)) {
			pv = fy_get(e, "path");
			ps = fy_castp(&pv, "");
			rc = fyai_sandbox_mode_parse(fy_get(e, "mode", "rw"), &mode);
			fyai_error_check(ctx, !rc,
				err_out, "sandbox: invalid mode '%s' for path '%s'",
				fy_get(e, "mode", "rw"), ps);
		} else {
			ps = fy_castp(&e, "");
		}
		resolved = fyai_sandbox_resolve_path(sb->root, ps);
		fyai_error_check(ctx, resolved, err_out,
				 "sandbox: could not resolve allow path '%s'", ps);
		if (fyai_shell_sandbox_allow(ctx, sb, resolved, mode))
			goto err_out;
	}
	/* A program that makes a user namespace writes the maps of /proc/self, with O_TRUNC. */
	if (ctx->home && ctx->home->userns) {
		resolved = strdup("/proc");
		fyai_error_check(ctx, resolved, err_out,
				 "sandbox: could not copy the /proc path");
		if (fyai_shell_sandbox_allow(ctx, sb, resolved, FYAI_SB_EDIT))
			goto err_out;
	}
	/* The home directory of the tool is its own to write, whatever is denied. */
	if (ctx->home) {
		resolved = strdup(ctx->home->dir);
		fyai_error_check(ctx, resolved, err_out,
				 "sandbox: could not copy the home directory path");
		if (fyai_shell_sandbox_allow(ctx, sb, resolved, FYAI_SB_RW))
			goto err_out;
		sp->mount = ctx->home->mount;
		sp->mount_n = ctx->home->mount_n;
	}
	sp->allow = sb->allow;

	/* Each present protocol mapping restricts its destination ports. */
	net = fy_get(cs, "network");
	tcp = fy_get(net, "tcp");
	udp = fy_get(net, "udp");
	if (fy_is_valid(tcp) && !fyai_sandbox_net_restrictable(-1) &&
	    ctx->cfg->sandbox_profile) {
		/* The parent reported it when it applied the configuration. */
		tcp = fy_invalid;
	}
	if (fy_is_valid(tcp)) {
		/* Check here because the child cannot report why it failed. */
		fyai_error_check(ctx, fyai_sandbox_net_restrictable(-1), err_out,
				 "sandbox: TCP egress cannot be restricted by this build or kernel");
		sp->restrict_tcp = true;
		ports = fy_get(tcp, "ports");
		n = fy_is_sequence(ports) ? fy_len(ports) : 0;
		if (n) {
			sb->tcp_ports = calloc(n, sizeof(*sb->tcp_ports));
			fyai_error_check(ctx, sb->tcp_ports, err_out,
					 "sandbox: could not allocate the TCP port list");
			fy_foreach(port, ports)
				sb->tcp_ports[sp->tcp_ports_n++] = (uint16_t)
					fy_cast(port, 0LL);
		}
		sp->tcp_ports = sb->tcp_ports;
		ports = fy_get(tcp, "bind_ports");
		if (fy_is_valid(ports)) {
			sp->restrict_tcp_bind = true;
			n = fy_is_sequence(ports) ? fy_len(ports) : 0;
			if (n) {
				sb->tcp_bind_ports = calloc(n, sizeof(*sb->tcp_bind_ports));
				fyai_error_check(ctx, sb->tcp_bind_ports, err_out,
						 "sandbox: could not allocate the TCP bind port list");
				fy_foreach(port, ports)
					sb->tcp_bind_ports[sp->tcp_bind_ports_n++] = (uint16_t)
						fy_cast(port, 0LL);
			}
			sp->tcp_bind_ports = sb->tcp_bind_ports;
		}
	}
	if (fy_is_valid(udp)) {
		/*
		 * Without Landlock ABI 10 a seccomp filter denies UDP when no port is
		 * permitted; else the parent said that UDP stays open.
		 */
		if (!fyai_sandbox_udp_restrictable(-1)) {
			if (!fyai_sandbox_udp_ports_listed(udp) && fyai_sandbox_udp_deniable())
				sp->deny_udp = true;
		} else {
			sp->restrict_udp = true;
			ports = fy_get(udp, "ports");
			n = fy_is_sequence(ports) ? fy_len(ports) : 0;
			if (n) {
				sb->udp_ports = calloc(n, sizeof(*sb->udp_ports));
				fyai_error_check(ctx, sb->udp_ports, err_out,
						 "sandbox: could not allocate the UDP port list");
				fy_foreach(port, ports)
					sb->udp_ports[sp->udp_ports_n++] = (uint16_t)
						fy_cast(port, 0LL);
			}
			sp->udp_ports = sb->udp_ports;
			ports = fy_get(udp, "bind_ports");
			n = fy_is_sequence(ports) ? fy_len(ports) : 0;
			if (n) {
				sb->udp_bind_ports = calloc(n, sizeof(*sb->udp_bind_ports));
				fyai_error_check(ctx, sb->udp_bind_ports, err_out,
						 "sandbox: could not allocate the UDP bind port list");
				fy_foreach(port, ports)
					sb->udp_bind_ports[sp->udp_bind_ports_n++] = (uint16_t)
						fy_cast(port, 0LL);
			}
			sp->udp_bind_ports = sb->udp_bind_ports;
		}
	}

	sp->list_root = fy_get(cs, "list_root", false);

	*specp = sp;
	return 0;
err_out:
	if (cand) {
		for (i = 0; i < cn; i++)
			free(cand[i]);
		free(cand);
	}
	fyai_shell_sandbox_end(sb);
	return -1;
}

/*
 * Build the sandbox of a tool. With `sandbox_on_failure: warn` a sandbox that
 * cannot be set up is reported once, with its cause, and the tool runs
 * without it: @specp is NULL, as for a disabled sandbox.
 */
int fyai_shell_sandbox_begin(struct fyai_ctx *ctx,
			     struct fyai_shell_sandbox *sb,
			     const struct fyai_sandbox_spec **specp)
{
	bool had_error;
	char *cause;
	size_t len;

	if (!ctx->cfg->sandbox_on_failure ||
	    strcmp(ctx->cfg->sandbox_on_failure, "warn"))
		return fyai_shell_sandbox_build(ctx, sb, specp);
	had_error = fyai_diag_got_error(fyai_ctx_diag(ctx));
	if (!fyai_shell_sandbox_build(ctx, sb, specp))
		return 0;
	/* An error that was pending before the call is not the cause of this one. */
	cause = had_error ? NULL : fyai_diag_string(fyai_ctx_diag(ctx));
	if (!had_error)
		fyai_diag_reset(fyai_ctx_diag(ctx));
	if (!ctx->sandbox_warned) {
		len = cause ? strlen(cause) : 0;
		while (len && (cause[len - 1] == '\n' || cause[len - 1] == ' '))
			cause[--len] = '\0';
		fyai_warning(ctx, "the sandbox could not be set up (%s); the tools run "
			     "without it because `sandbox_on_failure` is `warn`. Set "
			     "`sandbox_on_failure: fail` to stop the tools instead, or fix the "
			     "cause (doc/apparmor-userns.md)",
			     len ? cause : "no cause was reported");
		ctx->sandbox_warned = true;
	}
	free(cause);
	memset(sb, 0, sizeof(*sb));
	*specp = NULL;
	return 0;
}

void fyai_shell_sandbox_end(struct fyai_shell_sandbox *sb)
{
	size_t i;

	for (i = 0; i < sb->spec.deny_n; i++)
		free((char *)sb->deny[i]);
	for (i = 0; i < sb->spec.allow_n; i++)
		free((char *)sb->allow[i].path);
	free(sb->deny);
	free(sb->allow);
	free(sb->tcp_ports);
	free(sb->tcp_bind_ports);
	free(sb->udp_ports);
	free(sb->udp_bind_ports);
	for (i = 0; i < sb->hide_n; i++)
		free((char *)sb->hide[i]);
	free(sb->hide);
	free(sb->root);
	memset(sb, 0, sizeof(*sb));
}
