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
#include "fyai_auth.h"
#include "fyai_config.h"
#include "fyai_grammar.h"
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
	return fyai_grammar_compact_url(cfg, out, size);
}

static uint32_t fnv(uint32_t h, const char *s)
{
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return (h ^ 0xff) * 16777619u;	/* a separator between fields */
}

/* The program of a grammar that a local program serves. */
static const char *profile_program(const struct fyai_cfg *cfg)
{
	return fy_str_empty(cfg->claude_code_command) ? "claude" :
	       cfg->claude_code_command;
}

bool fyai_transport_profile_name(const struct fyai_cfg *cfg, const char *kind,
				 char *out, size_t size)
{
	char url[1024], cred[512];
	uint32_t h = 2166136261u;
	bool compact = !strcmp(kind, FYAI_TPC_COMPACT);

	/* A program serves only the model request. */
	if (fyai_grammar(cfg)->cli) {
		if (compact)
			return false;
		h = fnv(h, fyai_grammar(cfg)->name);
		h = fnv(h, profile_program(cfg));
		h = fnv(h, cfg->claude_code_api_billing ? "billing" : "");
		snprintf(out, size, "p-%08x", h);
		return true;
	}

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
	h = fnv(h, fyai_grammar(cfg)->key_header ? fyai_grammar(cfg)->name :
						  "other");
	h = fnv(h, cred);
	snprintf(out, size, "%s-%08x", compact ? "c" : "m", h);
	return true;
}

/*
 * The profile of a grammar that a local program serves. The transport runs
 * the program, so the program and its login stay out of every agent.
 */
static int add_program(struct fyai_transport_grant *grant,
		       const struct fyai_cfg *cfg, const char **why)
{
	struct fyai_transport_profile_spec spec = { 0 };
	char name[FYAI_TPC_NAME_MAX];
	int rc;

	if (!fyai_transport_profile_name(cfg, FYAI_TPC_MODEL, name, sizeof(name)))
		return 0;
	if (fyai_transport_grant_find(grant, name))
		return 0;
	spec.name = name;
	spec.program = profile_program(cfg);
	spec.api_billing = cfg->claude_code_api_billing;
	spec.auth = FYAI_TA_NONE;
	rc = fyai_transport_grant_add_spec(grant, &spec);
	if (rc) {
		*why = rc == -ENOMEM ? "out of memory" :
		       "the program is not one that the transport runs";
		return rc == -ENOMEM ? rc : -EINVAL;
	}
	return 0;
}

static int add_one(struct fyai_transport_grant *grant, const struct fyai_cfg *cfg,
		   const char *kind, const char **why)
{
	const struct fyai_grammar *grammar = fyai_grammar(cfg);
	const struct fyai_grammar_header *hdr;
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
		if (grammar->key_header) {
			spec.auth = FYAI_TA_HEADER;
			spec.header = grammar->key_header;
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
	for (hdr = grammar->headers; hdr->name; hdr++) {
		rc = fyai_transport_grant_add_header(grant, name, hdr->name,
						     hdr->value);
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

	if (fyai_grammar(cfg)->cli)
		return add_program(grant, cfg, why);
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

bool fyai_transport_url_profile_name(const char *url, const char *credential,
				     bool images, char *out, size_t size)
{
	uint32_t h = 2166136261u;

	if (fy_str_empty(url) || fy_str_empty(credential))
		return false;
	h = fnv(h, url);
	h = fnv(h, credential);
	h = fnv(h, images ? "images" : "");
	snprintf(out, size, "d-%08x", h);
	return true;
}

int fyai_transport_url_profile_add(struct fyai_transport_grant *grant,
				   const char *url, const char *credential,
				   const char *const *headers, bool images,
				   const char **why)
{
	struct fyai_transport_profile_spec spec = { 0 };
	char name[FYAI_TPC_NAME_MAX], *sep;
	char field[128];
	size_t i;
	int rc;

	if (!fyai_transport_url_profile_name(url, credential, images, name,
					     sizeof(name))) {
		*why = "the request has no URL or no credential source";
		return -EINVAL;
	}
	if (fyai_transport_grant_find(grant, name))
		return 0;
	spec.name = name;
	spec.url = url;
	spec.plain_http = !strncmp(url, "http://", 7);
	spec.auth = FYAI_TA_BEARER;
	spec.credential = credential;
	rc = fyai_transport_grant_add_spec(grant, &spec);
	if (rc) {
		*why = rc == -ENOMEM ? "out of memory" :
		       "the endpoint is not one that the transport accepts";
		return rc == -ENOMEM ? rc : -EINVAL;
	}
	for (i = 0; headers && headers[i]; i++) {
		sep = strstr(headers[i], ": ");
		if (!sep || (size_t)(sep - headers[i]) >= sizeof(field)) {
			*why = "a fixed header is not NAME: VALUE";
			return -EINVAL;
		}
		memcpy(field, headers[i], sep - headers[i]);
		field[sep - headers[i]] = '\0';
		rc = fyai_transport_grant_add_header(grant, name, field,
						     sep + 2);
		if (rc) {
			*why = "out of memory";
			return rc;
		}
	}
	return 0;
}
