/*
 * fyai_sandbox_profile.c - named sandbox profiles
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A profile is data: the `sandbox_profiles` key of config.yaml.sample holds the
 * built-in ones, which an arena stores when it starts from the sample, and the
 * `sandbox_profiles` key of the configuration holds the rest. The sample is the
 * fallback for a name that the configuration lacks. No profile is known to the C
 * code by name, except that `lockdown` takes the legacy key `sandbox_lockdown`.
 */

#define FYAI_MODULE FYAIEM_CONFIG

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <ctype.h>
#include <sys/utsname.h>

#include "fyai.h"
#include "fyai_sandbox_profile.h"
#include "fyai_yaml_flags.h"

/* FYAI_EMBEDDED_CONFIG[]: config.yaml.sample, which holds the built-in profiles. */
#include "embedded_config.inc"

/* Longest chain of `extends`. */
#define PROFILE_DEPTH_MAX	8

/* The built-in profiles: the mapping `sandbox_profiles` of the embedded sample. */
static fy_generic profile_builtin_doc(struct fyai_cfg *cfg)
{
	fy_generic_sized_string embedded;
	fy_generic doc;

	embedded.data = (const char *)FYAI_EMBEDDED_CONFIG;
	embedded.size = FYAI_EMBEDDED_CONFIG_LEN;
	doc = fy_parse(cfg->gb, embedded,
		       FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
	return fy_get(assert_valid_generic(doc,
		"Unable to parse the embedded sample (config.yaml.sample)"),
		"sandbox_profiles", fy_invalid);
}

/*
 * Write the value of the placeholder @name of @len characters to @out.
 * Returns the length written, or -1 when the name is unknown or too long.
 */
static int placeholder_value(const char *name, size_t len, char *out,
			     size_t size)
{
	struct utsname un;
	const char *v;
	size_t i;
	int n;

	if (uname(&un))
		return -1;
	if (len == 8 && !strncmp(name, "hostname", len))
		n = snprintf(out, size, "%s", un.nodename);
	else if (len == 2 && !strncmp(name, "os", len)) {
		for (v = un.sysname, i = 0; *v && i + 1 < size; v++, i++)
			out[i] = tolower((unsigned char)*v);
		out[i] = '\0';
		n = (int)i;
	} else if (len == 4 && !strncmp(name, "arch", len))
		n = snprintf(out, size, "%s", un.machine);
	else if (len == 13 && !strncmp(name, "host_specific", len))
		n = snprintf(out, size, "~/.fyai/hosts/%s", un.nodename);
	else
		return -1;
	return n < 0 || (size_t)n >= size ? -1 : n;
}

/* Expand the placeholders of @in into @out. Returns 0, or -1 with @bad set. */
static int expand_path(const char *in, char *out, size_t size,
		       char *bad, size_t badsize)
{
	const char *end;
	size_t used = 0, len;
	int n;

	while (*in) {
		if (in[0] == '{' && in[1] == '{') {
			end = strstr(in + 2, "}}");
			if (!end)
				goto err;
			len = (size_t)(end - (in + 2));
			n = placeholder_value(in + 2, len, out + used,
					      size - used);
			if (n < 0) {
				snprintf(bad, badsize, "%.*s", (int)len, in + 2);
				return -1;
			}
			used += n;
			in = end + 2;
			continue;
		}
		if (used + 1 >= size)
			goto err;
		out[used++] = *in++;
	}
	out[used] = '\0';
	return 0;
err:
	snprintf(bad, badsize, "%s", in);
	return -1;
}

/* Expand the paths of the deny or allow list @seq in the builder of @cfg. */
static fy_generic expand_list(struct fyai_cfg *cfg, fy_generic seq, bool allow,
			      const char *profile, bool *ok)
{
	fy_generic out = fy_sequence(cfg->gb), e, pv, item;
	char buf[4096], bad[128];
	const char *s;

	fy_foreach(e, seq) {
		pv = allow && fy_is_mapping(e) ? fy_get(e, "path", fy_invalid) : e;
		s = fy_castp(&pv, "");
		if (expand_path(s, buf, sizeof(buf), bad, sizeof(bad))) {
			fyai_cfg_error(cfg, "sandbox profile '%s': unknown placeholder or "
				       "path too long in '%s'", profile, bad);
			*ok = false;
			return out;
		}
		item = fy_value(cfg->gb, buf);
		if (allow && fy_is_mapping(e))
			item = fy_assoc(cfg->gb, e, fy_value(cfg->gb, "path"), item);
		out = fy_append(cfg->gb, out, item);
	}
	return out;
}

/* One protocol of a network policy: the keys of @over replace those of @base. */
static fy_generic overlay_proto(struct fyai_cfg *cfg, fy_generic base,
				fy_generic over)
{
	fy_generic ports, bind, proto;

	if (!fy_is_mapping(base) && !fy_is_mapping(over))
		return fy_invalid;
	ports = fy_get(over, "ports", fy_invalid);
	if (!fy_is_sequence(ports))
		ports = fy_get(base, "ports", fy_invalid);
	bind = fy_get(over, "bind_ports", fy_invalid);
	if (!fy_is_sequence(bind))
		bind = fy_get(base, "bind_ports", fy_invalid);
	proto = fy_mapping(cfg->gb, "ports",
			   fy_is_sequence(ports) ? ports : fy_sequence(cfg->gb));
	if (fy_is_sequence(bind))
		proto = fy_assoc(cfg->gb, proto, fy_value(cfg->gb, "bind_ports"), bind);
	return proto;
}

/* @over on top of @base: `allow` adds, every other key that @over names replaces. */
static fy_generic overlay(struct fyai_cfg *cfg, fy_generic base, fy_generic over)
{
	fy_generic deny = fy_get(over, "deny", fy_invalid);
	fy_generic allow = fy_get(base, "allow", fy_invalid), e;
	fy_generic desc = fy_get(over, "description", fy_invalid);
	fy_generic bnet = fy_get(base, "network", fy_invalid);
	fy_generic onet = fy_get(over, "network", fy_invalid);
	fy_generic tcp, udp, net, out, view = fy_get(over, "view", fy_invalid);
	fy_generic list_root = fy_get(over, "list_root", fy_invalid);
	fy_generic home = fy_get(over, "home", fy_invalid);
	fy_generic mounts = fy_get(over, "mounts", fy_invalid);
	fy_generic expose = fy_get(base, "expose", fy_invalid);

	if (!fy_is_sequence(deny))
		deny = fy_get(base, "deny", fy_invalid);
	if (!fy_is_sequence(allow))
		allow = fy_sequence(cfg->gb);
	fy_foreach(e, fy_get(over, "allow", fy_invalid))
		allow = fy_append(cfg->gb, allow, e);
	if (!fy_is_string(desc))
		desc = fy_get(base, "description", fy_invalid);
	tcp = overlay_proto(cfg, fy_get(bnet, "tcp", fy_invalid),
			    fy_get(onet, "tcp", fy_invalid));
	udp = overlay_proto(cfg, fy_get(bnet, "udp", fy_invalid),
			    fy_get(onet, "udp", fy_invalid));
	net = fy_mapping(cfg->gb);
	if (fy_is_valid(tcp))
		net = fy_assoc(cfg->gb, net, fy_value(cfg->gb, "tcp"), tcp);
	if (fy_is_valid(udp))
		net = fy_assoc(cfg->gb, net, fy_value(cfg->gb, "udp"), udp);
	out = fy_mapping(cfg->gb,
			 "deny", fy_is_sequence(deny) ? deny : fy_sequence(cfg->gb),
			 "allow", allow, "network", net);
	if (fy_is_string(desc))
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "description"), desc);
	if (!fy_is_bool(view))
		view = fy_get(base, "view", fy_invalid);
	if (fy_is_bool(view))
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "view"), view);
	if (!fy_is_bool(list_root))
		list_root = fy_get(base, "list_root", fy_invalid);
	if (fy_is_bool(list_root))
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "list_root"), list_root);
	if (!fy_is_mapping(home))
		home = fy_get(base, "home", fy_invalid);
	if (fy_is_mapping(home))
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "home"), home);
	if (!fy_is_sequence(expose))
		expose = fy_sequence(cfg->gb);
	/* `expose` adds to that of the profile that `extends`, as `allow` does. */
	fy_foreach(e, fy_get(over, "expose", fy_invalid))
		expose = fy_append(cfg->gb, expose, e);
	if (fy_len(expose))
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "expose"), expose);
	if (!fy_is_sequence(mounts))
		mounts = fy_get(base, "mounts", fy_invalid);
	if (fy_is_sequence(mounts))
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "mounts"), mounts);
	return out;
}

/* The mounts of a profile, with the placeholders of each `source` and `target` expanded. */
static fy_generic expand_mounts(struct fyai_cfg *cfg, fy_generic mounts,
				const char *profile, bool *ok)
{
	static const char *const keys[] = { "source", "target" };
	fy_generic out = fy_sequence(cfg->gb), e, v, item;
	char buf[4096], bad[128];
	const char *s;
	size_t i;

	if (!fy_is_sequence(mounts))
		return fy_invalid;
	fy_foreach(e, mounts) {
		item = e;
		for (i = 0; i < 2; i++) {
			v = fy_get(e, keys[i], fy_invalid);
			if (!fy_is_string(v))
				continue;
			s = fy_castp(&v, "");
			if (expand_path(s, buf, sizeof(buf), bad, sizeof(bad))) {
				fyai_cfg_error(cfg, "sandbox profile '%s': unknown placeholder or "
					       "path too long in '%s'", profile, bad);
				*ok = false;
				return out;
			}
			item = fy_assoc(cfg->gb, item, fy_value(cfg->gb, keys[i]),
					fy_value(cfg->gb, buf));
		}
		out = fy_append(cfg->gb, out, item);
	}
	return out;
}

/* The `home` of a profile with the placeholders of its `dir` and `copy` expanded. */
static fy_generic expand_home(struct fyai_cfg *cfg, fy_generic home,
			      const char *profile, bool *ok)
{
	fy_generic dir = fy_get(home, "dir", fy_invalid), out;
	char buf[4096], bad[128];
	const char *s;

	if (!fy_is_mapping(home))
		return fy_invalid;
	out = fy_mapping(cfg->gb, "private", fy_get(home, "private", false),
			 "method", fy_get(home, "method", "overlay"),
			 "copy", expand_list(cfg, fy_get(home, "copy", fy_invalid),
					     false, profile, ok),
			 "env", expand_list(cfg, fy_get(home, "env", fy_invalid),
					    false, profile, ok));
	if (fy_is_string(dir)) {
		s = fy_castp(&dir, "");
		if (expand_path(s, buf, sizeof(buf), bad, sizeof(bad))) {
			fyai_cfg_error(cfg, "sandbox profile '%s': unknown placeholder or "
				       "path too long in '%s'", profile, bad);
			*ok = false;
			return out;
		}
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "dir"),
			       fy_value(cfg->gb, buf));
	}
	return out;
}

/*
 * The profile @name before the placeholders are expanded. The profile of the
 * configuration is the one that counts; the built-in one stands in for a name
 * that the configuration lacks. A profile starts from the one that `extends`
 * names, or from nothing. `extends` its own name means the built-in profile.
 */
static fy_generic profile_compose(struct fyai_cfg *cfg, fy_generic root,
				  fy_generic builtin, const char *name,
				  int depth)
{
	fy_generic user = fy_get(fy_get(root, "sandbox_profiles", fy_invalid),
				 name, fy_invalid);
	fy_generic base = fy_mapping(cfg->gb), out;
	fy_generic parent;
	fy_generic legacy = fy_get(root, "sandbox_lockdown", fy_invalid);
	const char *pname;

	if (!fy_is_mapping(user))
		user = fy_get(builtin, name, fy_invalid);
	if (!fy_is_mapping(user)) {
		fyai_cfg_error(cfg, "sandbox profile '%s' is not defined", name);
		return fy_invalid;
	}
	parent = fy_get(user, "extends", fy_invalid);
	if (fy_is_string(parent)) {
		pname = fy_castp(&parent, "");
		if (!strcmp(pname, name)) {
			base = fy_get(builtin, name, fy_invalid);
		} else if (depth >= PROFILE_DEPTH_MAX) {
			fyai_cfg_error(cfg, "sandbox profile '%s': 'extends' is a cycle "
				       "or goes deeper than %d", name, PROFILE_DEPTH_MAX);
			return fy_invalid;
		} else {
			base = profile_compose(cfg, root, builtin, pname, depth + 1);
			if (fy_is_invalid(base))
				return fy_invalid;
		}
		if (!fy_is_mapping(base))
			base = fy_mapping(cfg->gb);
	}
	out = overlay(cfg, base, user);
	/* The legacy key of the lockdown profile, over the profile. */
	if (!strcmp(name, "lockdown") && fy_is_mapping(legacy))
		out = overlay(cfg, out, legacy);
	return out;
}

fy_generic fyai_sandbox_profile_policy(struct fyai_cfg *cfg, fy_generic root,
				       const char *name)
{
	fy_generic policy;
	bool ok = true;
	fy_generic deny, allow, home, out, mounts, e;

	policy = profile_compose(cfg, root, profile_builtin_doc(cfg), name, 0);
	if (fy_is_invalid(policy))
		return fy_invalid;
	deny = expand_list(cfg, fy_get(policy, "deny", fy_invalid), false, name, &ok);
	allow = expand_list(cfg, fy_get(policy, "allow", fy_invalid), true, name, &ok);
	if (!ok)
		return fy_invalid;
	home = expand_home(cfg, fy_get(policy, "home", fy_invalid), name, &ok);
	if (!ok)
		return fy_invalid;
	out = fy_mapping(cfg->gb, "enabled", true, "deny", deny, "allow", allow,
			 "network", fy_get(policy, "network", fy_invalid),
			 "view", fy_get(policy, "view", false),
			 "list_root", fy_get(policy, "list_root", false));
	if (fy_is_mapping(home))
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "home"), home);
	fy_foreach(e, fy_get(policy, "expose", fy_invalid)) {
		if (!fy_any_equal(e, "userns")) {
			fyai_cfg_error(cfg, "sandbox profile '%s': unknown exposure '%s'", name,
				       fy_str(e) ? fy_str(e) : "?");
			return fy_invalid;
		}
	}
	if (fy_is_sequence(fy_get(policy, "expose", fy_invalid)))
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "expose"),
			       fy_get(policy, "expose", fy_invalid));
	mounts = expand_mounts(cfg, fy_get(policy, "mounts", fy_invalid), name, &ok);
	if (!ok)
		return fy_invalid;
	if (fy_is_sequence(mounts))
		out = fy_assoc(cfg->gb, out, fy_value(cfg->gb, "mounts"), mounts);
	return out;
}

/* The description of the profile @name, from @user first, else @builtin. */
static const char *profile_desc(fy_generic *d, fy_generic user,
				fy_generic builtin, const char *name)
{
	*d = fy_get(fy_get(user, name, fy_invalid), "description", fy_invalid);
	if (!fy_is_string(*d))
		*d = fy_get(fy_get(builtin, name, fy_invalid), "description",
			    fy_invalid);
	return fy_is_string(*d) ? fy_castp(d, "") : NULL;
}

void fyai_sandbox_profile_names(struct fyai_cfg *cfg, fy_generic root,
				void (*fn)(void *arg, const char *name,
					   const char *desc),
				void *arg)
{
	fy_generic builtin = profile_builtin_doc(cfg);
	fy_generic user = fy_get(root, "sandbox_profiles", fy_invalid);
	fy_generic key, val, d;
	const char *name;

	fn(arg, FYAI_SANDBOX_PROFILE_NONE, "no sandbox: the tools are not confined");
	fy_foreach_key_value(key, val, builtin) {
		name = fy_castp(&key, "");
		fn(arg, name, profile_desc(&d, user, builtin, name));
	}
	fy_foreach_key_value(key, val, user) {
		name = fy_castp(&key, "");
		if (fy_is_mapping(fy_get(builtin, name, fy_invalid)))
			continue;
		fn(arg, name, profile_desc(&d, user, builtin, name));
	}
}

bool fyai_sandbox_udp_ports_listed(fy_generic udp)
{
	fy_generic ports = fy_get(udp, "ports", fy_invalid);
	fy_generic bind = fy_get(udp, "bind_ports", fy_invalid);

	return (fy_is_sequence(ports) && !fy_empty(ports)) ||
	       (fy_is_sequence(bind) && !fy_empty(bind));
}
