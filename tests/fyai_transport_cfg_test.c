/*
 * fyai_transport_cfg_test.c - tests for the profiles that a configuration needs
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_test.h"
#include "fyai_config.h"
#include "fyai_secret.h"
#include "fyai_transport_boot.h"
#include "fyai_transport_cfg.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(transport_cfg, credential_source, transport_cfg_credential_source)
FYAI_TEST_ENTRY(transport_cfg, profile_names, transport_cfg_profile_names)
FYAI_TEST_ENTRY(transport_cfg, responses_profiles, transport_cfg_responses_profiles)
FYAI_TEST_ENTRY(transport_cfg, messages_profile, transport_cfg_messages_profile)
FYAI_TEST_ENTRY(transport_cfg, local_endpoints, transport_cfg_local_endpoints)
FYAI_TEST_ENTRY(transport_cfg, refuses_what_it_cannot_isolate, transport_cfg_refuses_what_it_cannot_isolate)
FYAI_TEST_ENTRY(transport_cfg, providers_share_a_set, transport_cfg_providers_share_a_set)
FYAI_TEST_ENTRY(transport_cfg, isolation_change_restarts, transport_cfg_isolation_change_restarts)
FYAI_TEST_ENTRY(transport_cfg, secret_stays_at_the_transport, transport_cfg_secret_stays_at_the_transport)

static struct fy_generic_builder *new_gb(void)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&cfg);

	FYAI_TCHECK(gb);
	return gb;
}

static void openai(struct fyai_cfg *cfg, struct fy_generic_builder *gb)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->gb = gb;
	cfg->provider = "openai";
	cfg->api_mode = FYAI_API_RESPONSES;
	cfg->api_url = "https://api.openai.com/v1/responses";
	cfg->api_key_auto = true;
}

static void anthropic(struct fyai_cfg *cfg, struct fy_generic_builder *gb)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->gb = gb;
	cfg->provider = "anthropic";
	cfg->api_mode = FYAI_API_MESSAGES;
	cfg->api_url = "https://api.anthropic.com/v1/messages";
	cfg->api_key_auto = true;
}

int transport_cfg_credential_source(void)
{
	struct fy_generic_builder *gb = new_gb();
	struct fyai_cfg cfg;
	char src[256];

	/* The provider default tries its variable, then the secret store. */
	openai(&cfg, gb);
	FYAI_TCHECK(fyai_transport_credential_source(&cfg, src, sizeof(src)));
	FYAI_TCHECK(!strcmp(src, "env:OPENAI_API_KEY|secret:api-key/openai"));

	/* A provider with punctuation in its name gets the usual variable. */
	cfg.provider = "open-router";
	FYAI_TCHECK(fyai_transport_credential_source(&cfg, src, sizeof(src)));
	FYAI_TCHECK(!strcmp(src, "env:OPEN_ROUTER_API_KEY|secret:api-key/open-router"));

	/* An explicit reference wins, and --api-key is a memory source. */
	cfg.api_key_ref = "env:MY_KEY";
	FYAI_TCHECK(fyai_transport_credential_source(&cfg, src, sizeof(src)));
	FYAI_TCHECK(!strcmp(src, "env:MY_KEY"));
	cfg.api_key_ref = "secret:team";
	FYAI_TCHECK(fyai_transport_credential_source(&cfg, src, sizeof(src)));
	FYAI_TCHECK(!strcmp(src, "secret:team"));
	cfg.api_key_ref = "mem:cli";
	FYAI_TCHECK(fyai_transport_credential_source(&cfg, src, sizeof(src)));
	FYAI_TCHECK(!strcmp(src, "mem:cli"));

	/* No authentication, no credential; and none without a provider. */
	cfg.no_auth = true;
	FYAI_TCHECK(!fyai_transport_credential_source(&cfg, src, sizeof(src)) && !*src);
	cfg.no_auth = false;
	cfg.api_key_ref = NULL;
	cfg.provider = NULL;
	FYAI_TCHECK(!fyai_transport_credential_source(&cfg, src, sizeof(src)));
	fy_generic_builder_destroy(gb);
	return 0;
}

int transport_cfg_profile_names(void)
{
	struct fy_generic_builder *gb = new_gb();
	struct fyai_cfg a, b;
	char na[FYAI_TPC_NAME_MAX], nb[FYAI_TPC_NAME_MAX], nc[FYAI_TPC_NAME_MAX];
	struct fyai_transport_grant g = { 0 };

	openai(&a, gb);
	openai(&b, gb);
	FYAI_TCHECK(fyai_transport_profile_name(&a, FYAI_TPC_MODEL, na, sizeof(na)));
	FYAI_TCHECK(fyai_transport_profile_name(&b, FYAI_TPC_MODEL, nb, sizeof(nb)));
	/* The same endpoint and credential: the same profile, so agents share it. */
	FYAI_TCHECK(!strcmp(na, nb));

	/* The name is valid for a profile, and a compact profile has its own. */
	FYAI_TCHECK(!fyai_transport_grant_add(&g, na, "https://a.example/", NULL,
					      NULL, FYAI_TA_NONE, NULL, NULL));
	fyai_transport_grant_clear(&g);
	FYAI_TCHECK(fyai_transport_profile_name(&a, FYAI_TPC_COMPACT, nc, sizeof(nc)));
	FYAI_TCHECK(strcmp(na, nc));

	/* Anything that changes where the request goes changes the name. */
	b.api_url = "https://other.example/v1/responses";
	FYAI_TCHECK(fyai_transport_profile_name(&b, FYAI_TPC_MODEL, nb, sizeof(nb)));
	FYAI_TCHECK(strcmp(na, nb));
	openai(&b, gb);
	b.api_key_ref = "env:OTHER";
	FYAI_TCHECK(fyai_transport_profile_name(&b, FYAI_TPC_MODEL, nb, sizeof(nb)));
	FYAI_TCHECK(strcmp(na, nb));
	anthropic(&b, gb);
	FYAI_TCHECK(fyai_transport_profile_name(&b, FYAI_TPC_MODEL, nb, sizeof(nb)));
	FYAI_TCHECK(strcmp(na, nb));

	/* The compact endpoint exists only for a Responses URL. */
	FYAI_TCHECK(!fyai_transport_profile_name(&b, FYAI_TPC_COMPACT, nb, sizeof(nb)));
	openai(&b, gb);
	b.api_url = "https://x.example/v1/chat";
	FYAI_TCHECK(!fyai_transport_profile_name(&b, FYAI_TPC_COMPACT, nb, sizeof(nb)));
	FYAI_TCHECK(!fyai_transport_profile_name(&b, "other", nb, sizeof(nb)) ||
		    strcmp(nb, na));
	fy_generic_builder_destroy(gb);
	return 0;
}

int transport_cfg_responses_profiles(void)
{
	struct fy_generic_builder *gb = new_gb();
	struct fyai_transport_grant g = { 0 };
	const struct fyai_transport_profile *pm, *pc;
	char nm[FYAI_TPC_NAME_MAX], nc[FYAI_TPC_NAME_MAX], url[128];
	const char *why = "";
	struct fyai_cfg cfg;

	openai(&cfg, gb);
	FYAI_TCHECK(!fyai_transport_profiles_add(&g, &cfg, &why));
	FYAI_TCHECK(g.count == 2);
	FYAI_TCHECK(fyai_transport_profile_name(&cfg, FYAI_TPC_MODEL, nm, sizeof(nm)));
	FYAI_TCHECK(fyai_transport_profile_name(&cfg, FYAI_TPC_COMPACT, nc, sizeof(nc)));
	pm = fyai_transport_grant_find(&g, nm);
	pc = fyai_transport_grant_find(&g, nc);
	FYAI_TCHECK(pm && pc);
	FYAI_TCHECK(!strcmp(pm->url, "https://api.openai.com/v1/responses"));
	FYAI_TCHECK(!strcmp(pc->url, "https://api.openai.com/v1/responses/compact"));
	FYAI_TCHECK(fyai_transport_compact_url(&cfg, url, sizeof(url)) && !strcmp(pc->url, url));
	FYAI_TCHECK(pm->auth == FYAI_TA_BEARER && !pm->plain_http && !pm->header);
	FYAI_TCHECK(!strcmp(pm->credential, "env:OPENAI_API_KEY|secret:api-key/openai"));
	FYAI_TCHECK(!strcmp(pc->credential, pm->credential));

	/* Adding again changes nothing. */
	FYAI_TCHECK(!fyai_transport_profiles_add(&g, &cfg, &why) && g.count == 2);
	fyai_transport_grant_clear(&g);
	fy_generic_builder_destroy(gb);
	return 0;
}

int transport_cfg_messages_profile(void)
{
	struct fy_generic_builder *gb = new_gb();
	struct fyai_transport_grant g = { 0 };
	const struct fyai_transport_profile *p;
	char n[FYAI_TPC_NAME_MAX];
	const char *why = "";
	struct fyai_cfg cfg;

	anthropic(&cfg, gb);
	cfg.api_key_ref = "secret:anth";
	FYAI_TCHECK(!fyai_transport_profiles_add(&g, &cfg, &why));
	FYAI_TCHECK(g.count == 1);	/* Messages has no compact endpoint */
	FYAI_TCHECK(fyai_transport_profile_name(&cfg, FYAI_TPC_MODEL, n, sizeof(n)));
	p = fyai_transport_grant_find(&g, n);
	FYAI_TCHECK(p && p->auth == FYAI_TA_HEADER && !strcmp(p->header, "x-api-key"));
	FYAI_TCHECK(!strcmp(p->credential, "secret:anth"));
	FYAI_TCHECK(p->nheaders == 1 && !strcmp(p->headers[0], "anthropic-version: " ANTHROPIC_VERSION));
	fyai_transport_grant_clear(&g);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* A local model server: loopback or on a network, over http, with or without a key. */
int transport_cfg_local_endpoints(void)
{
	struct fy_generic_builder *gb = new_gb();
	struct fyai_transport_grant g = { 0 };
	const struct fyai_transport_profile *p;
	char n[FYAI_TPC_NAME_MAX];
	const char *why = "";
	struct fyai_cfg cfg;

	openai(&cfg, gb);
	cfg.api_mode = FYAI_API_CHAT_COMPLETIONS;
	cfg.api_url = "http://ollama.lan:11434/v1/chat/completions";
	cfg.no_auth = true;
	FYAI_TCHECK(!fyai_transport_profiles_add(&g, &cfg, &why));
	FYAI_TCHECK(fyai_transport_profile_name(&cfg, FYAI_TPC_MODEL, n, sizeof(n)));
	p = fyai_transport_grant_find(&g, n);
	FYAI_TCHECK(p && p->auth == FYAI_TA_NONE && p->plain_http && !p->credential);
	fyai_transport_grant_clear(&g);

	cfg.no_auth = false;
	cfg.api_key_ref = "env:LOCAL_KEY";
	cfg.api_url = "http://127.0.0.1:8080/v1/chat/completions";
	FYAI_TCHECK(!fyai_transport_profiles_add(&g, &cfg, &why));
	FYAI_TCHECK(g.count == 1);
	fyai_transport_grant_clear(&g);
	fy_generic_builder_destroy(gb);
	return 0;
}

int transport_cfg_refuses_what_it_cannot_isolate(void)
{
	struct fy_generic_builder *gb = new_gb();
	struct fyai_transport_grant g = { 0 };
	const char *why = "";
	struct fyai_cfg cfg;

	openai(&cfg, gb);
	cfg.api_url = NULL;
	FYAI_TCHECK(fyai_transport_profiles_add(&g, &cfg, &why) == -EINVAL);
	cfg.api_url = "ftp://x.example/responses";
	FYAI_TCHECK(fyai_transport_profiles_add(&g, &cfg, &why) == -EINVAL);
	cfg.api_url = "https://api.openai.com/v1/responses";
	cfg.provider = NULL;
	FYAI_TCHECK(fyai_transport_profiles_add(&g, &cfg, &why) == -EINVAL);
	FYAI_TCHECK(strstr(why, "credential") && !g.count);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* Two providers in one grant set: each agent is granted only its own. */
int transport_cfg_providers_share_a_set(void)
{
	struct fy_generic_builder *gb = new_gb();
	struct fyai_transport_grant g = { 0 };
	char names[FYAI_TPC_KINDS][FYAI_TPC_NAME_MAX];
	struct fyai_transport_allow allow[FYAI_TPC_KINDS];
	const char *why = "";
	struct fyai_cfg a, b;
	size_t n, i;

	openai(&a, gb);
	anthropic(&b, gb);
	FYAI_TCHECK(!fyai_transport_profiles_add(&g, &a, &why));
	FYAI_TCHECK(!fyai_transport_profiles_add(&g, &b, &why));
	FYAI_TCHECK(g.count == 3);	/* model+compact, and model */

	n = fyai_transport_allow_for(&a, names, allow);
	FYAI_TCHECK(n == 2);
	for (i = 0; i < n; i++)
		FYAI_TCHECK(fyai_transport_grant_find(&g, allow[i].profile));
	n = fyai_transport_allow_for(&b, names, allow);
	FYAI_TCHECK(n == 1 && fyai_transport_grant_find(&g, allow[0].profile));
	fyai_transport_grant_clear(&g);
	fy_generic_builder_destroy(gb);
	return 0;
}

/*
 * A supervised agent keeps the reference to a secret and never reads the store:
 * only the transport resolves it. The same configuration outside isolation
 * resolves the value.
 */
int transport_cfg_secret_stays_at_the_transport(void)
{
	struct fy_generic_builder *gb = new_gb();
	struct fyai_cfg cfg;
	char name[64];
	const char *value = "sk-secret-store-0123456789";
	fy_generic root;
	int rc;

#ifndef __linux__
	/* The secret store of another platform is the keychain of the user. */
	fy_generic_builder_destroy(gb);
	return 0;
#endif
	snprintf(name, sizeof(name), "fyai:transport-test-%d", (int)getpid());
	rc = fyai_secret_kernel_set(name, value, strlen(value));
	if (rc != FYAI_SECRET_OK) {
		fy_generic_builder_destroy(gb);
		return 0;	/* no secret store here */
	}
	root = fy_mapping(gb, "api_key", fy_mapping(gb, "type", "secret",
				"value", name + strlen("fyai:")));

	unsetenv(FYAI_TRANSPORT_FD_ENV);
	openai(&cfg, gb);
	FYAI_TCHECK(!fyai_config_apply(&cfg, root));
	FYAI_TCHECK(cfg.api_key && !strcmp(cfg.api_key, value));

	setenv(FYAI_TRANSPORT_FD_ENV, "3", 1);
	openai(&cfg, gb);
	FYAI_TCHECK(!fyai_config_apply(&cfg, root));
	FYAI_TCHECK(!cfg.api_key);
	FYAI_TCHECK(cfg.api_key_ref && !strncmp(cfg.api_key_ref, "secret:", 7));
	unsetenv(FYAI_TRANSPORT_FD_ENV);

	(void)fyai_secret_kernel_delete(name);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* A changed isolation level restarts the session, unless the environment fixes it. */
int transport_cfg_isolation_change_restarts(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	const char *why = NULL;

	unsetenv("FYAI_TRANSPORT_FORCED");
	cfg.agent_transport_isolation = "level-b";
	FYAI_TCHECK(fyai_transport_config_changed(&ctx, &why) == 1);
	cfg.agent_transport_isolation = "auto";
	FYAI_TCHECK(fyai_transport_config_changed(&ctx, &why) == 1);
	cfg.agent_transport_isolation = "none";
	FYAI_TCHECK(fyai_transport_config_changed(&ctx, &why) == 0);

	/* A transport is running: it is the `none` that restarts. */
	ctx.tclient = (struct fyai_tclient *)&ctx;
	FYAI_TCHECK(fyai_transport_config_changed(&ctx, &why) == 1);
	cfg.agent_transport_isolation = "level-b";
	FYAI_TCHECK(fyai_transport_config_changed(&ctx, &why) == 0);

	setenv("FYAI_TRANSPORT_FORCED", "1", 1);
	cfg.agent_transport_isolation = "none";
	FYAI_TCHECK(fyai_transport_config_changed(&ctx, &why) == -1 && why);
	unsetenv("FYAI_TRANSPORT_FORCED");
	return 0;
}
