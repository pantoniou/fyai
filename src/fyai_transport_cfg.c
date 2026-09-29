/*
 * fyai_transport_cfg.c - the transport profiles that a configuration needs
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fyai.h"
#include "fyai_config.h"
#include "fyai_transport_cfg.h"

bool fyai_transport_credential_source(const struct fyai_cfg *cfg, char *out,
				      size_t size)
{
	struct fy_generic_builder *gb;
	const char *env;

	if (cfg->no_auth) {
		if (size)
			out[0] = '\0';
		return false;
	}
	if (cfg->api_key_ref && *cfg->api_key_ref) {
		snprintf(out, size, "%s", cfg->api_key_ref);
		return true;
	}
	if (!cfg->provider || !*cfg->provider) {
		if (size)
			out[0] = '\0';
		return false;
	}
	/* The provider default, in the order that the configuration tries. */
	gb = cfg->gb;
	env = gb ? fyai_config_provider_env_key(gb, cfg->provider) : NULL;
	if (env)
		snprintf(out, size, "env:%s|secret:api-key/%s", env,
			 cfg->provider);
	else
		snprintf(out, size, "secret:api-key/%s", cfg->provider);
	return true;
}

bool fyai_transport_compact_url(const struct fyai_cfg *cfg, char *out, size_t size)
{
	static const char suffix[] = "/responses";
	size_t len;

	if (!cfg->api_url || cfg->api_mode != FYAI_API_RESPONSES)
		return false;
	len = strlen(cfg->api_url);
	if (len < sizeof(suffix) - 1 ||
	    strcmp(cfg->api_url + len - (sizeof(suffix) - 1), suffix))
		return false;
	return snprintf(out, size, "%s/compact", cfg->api_url) < (int)size;
}

static uint32_t fnv(uint32_t h, const char *s)
{
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return (h ^ 0xff) * 16777619u;	/* a separator between fields */
}

bool fyai_transport_profile_name(const struct fyai_cfg *cfg, const char *kind,
				 char *out, size_t size)
{
	char url[1024], cred[512];
	uint32_t h = 2166136261u;
	bool compact = !strcmp(kind, FYAI_TPC_COMPACT);

	if (compact) {
		if (!fyai_transport_compact_url(cfg, url, sizeof(url)))
			return false;
	} else {
		if (!cfg->api_url || snprintf(url, sizeof(url), "%s", cfg->api_url) >= (int)sizeof(url))
			return false;
	}
	if (!fyai_transport_credential_source(cfg, cred, sizeof(cred)))
		cred[0] = '\0';
	h = fnv(h, url);
	h = fnv(h, cfg->api_mode == FYAI_API_MESSAGES ? "messages" : "other");
	h = fnv(h, cred);
	snprintf(out, size, "%s-%08x", compact ? "c" : "m", h);
	return true;
}

static int add_one(struct fyai_transport_grant *grant, const struct fyai_cfg *cfg,
		   const char *kind, const char **why)
{
	struct fyai_transport_profile_spec spec = { 0 };
	char name[FYAI_TPC_NAME_MAX], url[1024], cred[512];
	int rc;

	if (!fyai_transport_profile_name(cfg, kind, name, sizeof(name)))
		return 0;			/* this endpoint has no such kind */
	if (fyai_transport_grant_find(grant, name))
		return 0;
	if (!strcmp(kind, FYAI_TPC_COMPACT))
		fyai_transport_compact_url(cfg, url, sizeof(url));
	else
		snprintf(url, sizeof(url), "%s", cfg->api_url);

	spec.name = name;
	spec.url = url;
	spec.plain_http = !strncmp(url, "http://", 7);
	if (cfg->no_auth) {
		spec.auth = FYAI_TA_NONE;
	} else {
		if (!fyai_transport_credential_source(cfg, cred, sizeof(cred))) {
			*why = "the configuration names no credential source";
			return -EINVAL;
		}
		spec.credential = cred;
		if (cfg->api_mode == FYAI_API_MESSAGES) {
			spec.auth = FYAI_TA_HEADER;
			spec.header = "x-api-key";
		} else {
			spec.auth = FYAI_TA_BEARER;
		}
	}
	rc = fyai_transport_grant_add_spec(grant, &spec);
	if (rc) {
		*why = rc == -ENOMEM ? "out of memory" :
		       "the endpoint is not one that the transport accepts";
		return rc == -ENOMEM ? rc : -EINVAL;
	}
	if (cfg->api_mode == FYAI_API_MESSAGES) {
		rc = fyai_transport_grant_add_header(grant, name,
						     "anthropic-version",
						     ANTHROPIC_VERSION);
		if (rc) {
			*why = "out of memory";
			return rc;
		}
	}
	return 0;
}

int fyai_transport_profiles_add(struct fyai_transport_grant *grant,
				const struct fyai_cfg *cfg, const char **why)
{
	int rc;

	if (cfg->chatgpt_auth) {
		*why = "the ChatGPT subscription login keeps its tokens in the "
		       "agent and cannot run with credential isolation";
		return -ENOTSUP;
	}
	if (!cfg->api_url || !*cfg->api_url) {
		*why = "the configuration has no API URL";
		return -EINVAL;
	}
	rc = add_one(grant, cfg, FYAI_TPC_MODEL, why);
	if (rc)
		return rc;
	return add_one(grant, cfg, FYAI_TPC_COMPACT, why);
}

size_t fyai_transport_allow_for(const struct fyai_cfg *cfg,
				char names[FYAI_TPC_KINDS][FYAI_TPC_NAME_MAX],
				struct fyai_transport_allow allow[FYAI_TPC_KINDS])
{
	static const char *const kinds[FYAI_TPC_KINDS] = {
		FYAI_TPC_MODEL, FYAI_TPC_COMPACT,
	};
	size_t n = 0;
	int i;

	for (i = 0; i < FYAI_TPC_KINDS; i++) {
		if (!fyai_transport_profile_name(cfg, kinds[i], names[n],
						 FYAI_TPC_NAME_MAX))
			continue;
		allow[n].profile = names[n];
		allow[n].model = NULL;
		n++;
	}
	return n;
}
