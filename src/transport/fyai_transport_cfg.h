/*
 * fyai_transport_cfg.h - the transport profiles that a configuration needs
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A configuration says where its model requests go and which credential
 * authenticates them. This module turns that into egress profiles for the
 * transport and names them, so that the supervisor and each agent agree on the
 * names without a table. A profile name comes from the endpoint, the grammar,
 * and the credential source: two agents on the same provider share a profile,
 * and an agent on another provider gets its own.
 *
 * Nothing here reads a credential.
 */

#ifndef FYAI_TRANSPORT_CFG_H
#define FYAI_TRANSPORT_CFG_H

#include <stddef.h>

#include "fyai_transport.h"

struct fyai_cfg;

#define FYAI_TPC_NAME_MAX	64
#define FYAI_TPC_KINDS		2

/* Profile kinds. The model request, and the Responses compact request. */
#define FYAI_TPC_MODEL		"model"
#define FYAI_TPC_COMPACT	"compact"

/*
 * Write the credential source of @cfg: "env:NAME", "secret:NAME",
 * "mem:cli", or the chain of the provider default, "env:X_API_KEY|secret:api-key/x".
 * Return false if the configuration takes no credential.
 */
bool fyai_transport_credential_source(const struct fyai_cfg *cfg, char *out,
				      size_t size);

/*
 * Write the name of the profile of @kind for @cfg. Return false if @cfg has no
 * such profile: the compact endpoint exists only for a Responses URL.
 */
bool fyai_transport_profile_name(const struct fyai_cfg *cfg, const char *kind,
				 char *out, size_t size);

/*
 * Add to @grant the profiles that @cfg needs and that it lacks. Return 0,
 * -ENOTSUP when the configuration cannot run isolated (the ChatGPT
 * subscription login keeps its tokens in the agent), -EINVAL when the endpoint
 * is not one that the transport accepts, or -ENOMEM. @why then names the reason
 * in static storage.
 */
int fyai_transport_profiles_add(struct fyai_transport_grant *grant,
				const struct fyai_cfg *cfg, const char **why);

/*
 * Fill @allow with the grant of an agent that runs @cfg, in @names storage.
 * Return the number of entries, at most FYAI_TPC_KINDS.
 */
size_t fyai_transport_allow_for(const struct fyai_cfg *cfg,
				char names[FYAI_TPC_KINDS][FYAI_TPC_NAME_MAX],
				struct fyai_transport_allow allow[FYAI_TPC_KINDS]);

/* The URL of the compact endpoint for @cfg, or false. */
bool fyai_transport_compact_url(const struct fyai_cfg *cfg, char *out, size_t size);

#endif
