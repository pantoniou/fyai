/*
 * fyai_transient.c - the transient builder component
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <string.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-allocator.h>
#include <libfyaml/libfyaml-generic.h>

#include "fyai_transient.h"

void fyai_transient_init(struct fyai_transient *sc)
{
	memset(sc, 0, sizeof(*sc));
}

int fyai_transient_setup(struct fyai_transient *sc)
{
	struct fy_auto_allocator_cfg trans_cfg;
	struct fy_generic_builder_cfg gb_cfg;

	/* An explicit owner adopts a builder that fyai_transient_get() made: the
	 * idle loop must not release it while the owner works. */
	if (sc->gb) {
		sc->autorelease = false;
		return 0;
	}
	memset(&trans_cfg, 0, sizeof(trans_cfg));
	trans_cfg.scenario = FYAST_PER_TAG_FREE_DEDUP;
	trans_cfg.estimated_max_size = 0;
	sc->allocator = fy_allocator_create("auto", &trans_cfg);
	assert(sc->allocator);

	memset(&gb_cfg, 0, sizeof(gb_cfg));
	gb_cfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED |
		       FYGBCF_CREATE_TAG;
	gb_cfg.allocator = sc->allocator;
	gb_cfg.parent = sc->parent_gb ? *sc->parent_gb : NULL;
	sc->gb = fy_generic_builder_create(&gb_cfg);
	assert(sc->gb);
	return 0;
}

void fyai_transient_cleanup(struct fyai_transient *sc)
{
	if (sc->gb) {
		if (sc->on_release)
			sc->on_release(sc->on_release_data);
		fy_generic_builder_destroy(sc->gb);
		sc->gb = NULL;
	}
	if (sc->allocator) {
		fy_allocator_destroy(sc->allocator);
		sc->allocator = NULL;
	}
	sc->autorelease = false;
}

struct fy_generic_builder *fyai_transient_get(struct fyai_transient *sc)
{
	if (sc->gb)
		return sc->gb;
	if (fyai_transient_setup(sc))
		return NULL;
	sc->autorelease = true;
	return sc->gb;
}
