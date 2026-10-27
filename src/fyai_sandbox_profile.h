/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SANDBOX_PROFILE_H
#define FYAI_SANDBOX_PROFILE_H

#include "fyai.h"

/* The name that selects no profile: the tools are not confined. */
#define FYAI_SANDBOX_PROFILE_NONE	"none"
/* The profile that a configuration names when it names none. */
#define FYAI_SANDBOX_PROFILE_DEFAULT	"dev"

/*
 * Resolve the sandbox profile @name against the configuration @root and return
 * its policy: {enabled, description, deny, allow, network}, built in the
 * builder of @cfg. A profile is the entry of `sandbox_profiles` of the
 * configuration, else the built-in one of that name from config.yaml.sample. A
 * key that it names replaces the one of the profile that `extends` names, except
 * `allow`, which adds to it; with no `extends` it starts from nothing, and
 * `extends` its own name starts from the built-in profile. The legacy key
 * `sandbox_lockdown` is applied to `lockdown` over the result.
 * `view` says whether the session runs in a view of the project; it is false
 * when no profile names it.
 * A path may use {{hostname}}, {{os}}, {{arch}} and {{host_specific}}.
 * Returns fy_invalid and reports the cause when the name is unknown, a chain of
 * `extends` is a cycle, or a placeholder is unknown.
 */
fy_generic fyai_sandbox_profile_policy(struct fyai_cfg *cfg, fy_generic root,
				       const char *name);

/*
 * Call @fn once for each profile name, `normal` first, the built-in ones and
 * then those of @root. @desc is NULL when the profile has no description.
 */
void fyai_sandbox_profile_names(struct fyai_cfg *cfg, fy_generic root,
				void (*fn)(void *arg, const char *name,
					   const char *desc),
				void *arg);

/*
 * True when the `network.udp` mapping @udp permits a destination or a local
 * port. A seccomp filter denies UDP whole and cannot enforce such a list.
 */
bool fyai_sandbox_udp_ports_listed(fy_generic udp);

#endif
