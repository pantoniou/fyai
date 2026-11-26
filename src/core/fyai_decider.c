/*
 * fyai_decider.c - the deciders: canned and configured
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_UNKNOWN
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fnmatch.h>
#include <string.h>

#include "fyai.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_decider.h"
#include "fyai_yaml_flags.h"

/* The embedded data/decisions.yaml document. */
#include "embedded_decisions.inc"

/* A decider extends another at most this deep. */
#define DECIDER_USE_DEPTH	4

static fy_generic decider_canned(struct fyai_cfg *cfg)
{
	fy_generic doc;
	fy_generic_sized_string embedded;

	if (fy_is_mapping(cfg->canned_decisions))
		return cfg->canned_decisions;
	embedded.data = (const char *)FYAI_EMBEDDED_DECISIONS;
	embedded.size = FYAI_EMBEDDED_DECISIONS_LEN;
	doc = fy_parse(cfg->gb, embedded,
		       FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
	fyai_cfg_error_check(cfg, fy_is_mapping(doc), err_out,
			     "cannot parse the built-in decision definitions");
	cfg->canned_decisions = doc;
	return doc;

err_out:
	return fy_map_empty;
}

static fy_generic decider_configured(struct fyai_cfg *cfg)
{
	fy_generic v = fy_get(cfg->config_doc, "decisions", fy_invalid);

	return fy_is_mapping(v) ? v : fy_map_empty;
}

/* A canned option by name, or fy_invalid. */
static fy_generic decider_canned_option(struct fyai_cfg *cfg, fy_generic name)
{
	return fy_get(fy_get(decider_canned(cfg), "options", fy_map_empty),
		      name, fy_invalid);
}

/* One option: a canned name, {use: NAME, ...overrides}, or a definition. */
static fy_generic decider_option(struct fyai_cfg *cfg,
				 struct fy_generic_builder *gb,
				 fy_generic key, fy_generic v,
				 const char **whyp)
{
	fy_generic use, base, k, x;

	if (fy_is_null(v) || fy_is_invalid(v))
		v = key;
	if (fy_is_string(v)) {
		base = decider_canned_option(cfg, v);
		if (!fy_is_mapping(base)) {
			*whyp = fy_gb_intern_string(gb, fy_sprintfa(
				"no canned option '%s'", fy_castp(&v, "?")));
			return fy_invalid;
		}
		return base;
	}
	if (!fy_is_mapping(v)) {
		*whyp = fy_gb_intern_string(gb, fy_sprintfa(
			"the option '%s' is not a mapping", fy_castp(&key, "?")));
		return fy_invalid;
	}
	use = fy_get(v, "use", fy_invalid);
	if (fy_is_invalid(use))
		return v;
	base = decider_canned_option(cfg, use);
	if (!fy_is_mapping(base)) {
		*whyp = fy_gb_intern_string(gb, fy_sprintfa(
			"no canned option '%s'", fy_castp(&use, "?")));
		return fy_invalid;
	}
	fy_foreach_key_value(k, x, v)
		if (!fy_equal(k, "use"))
			base = fy_assoc(gb, base, k, x);
	return base;
}

fy_generic fyai_decider_options(struct fyai_cfg *cfg,
				struct fy_generic_builder *gb,
				fy_generic options, const char **whyp)
{
	fy_generic out = fy_map_empty, key, v, opt;

	if (fy_is_sequence(options)) {
		fy_foreach(key, options) {
			if (!fy_is_string(key)) {
				*whyp = "a list of options names canned options";
				return fy_invalid;
			}
			opt = decider_option(cfg, gb, key, fy_invalid, whyp);
			if (fy_is_invalid(opt))
				return fy_invalid;
			out = fy_assoc(gb, out, key, opt);
		}
	} else if (fy_is_mapping(options)) {
		fy_foreach_key_value(key, v, options) {
			opt = decider_option(cfg, gb, key, v, whyp);
			if (fy_is_invalid(opt))
				return fy_invalid;
			out = fy_assoc(gb, out, key, opt);
		}
	} else {
		*whyp = "the options are a list of canned names or a mapping";
		return fy_invalid;
	}
	if (!fy_len(out)) {
		*whyp = "the decider has no options";
		return fy_invalid;
	}
	return out;
}

/* The spec of @name, configured before canned, with its `use` folded in. */
static fy_generic decider_spec(struct fyai_cfg *cfg,
			       struct fy_generic_builder *gb, fy_generic name,
			       int depth, const char **whyp)
{
	fy_generic spec, use, base, k, v, cuse, cbase;

	spec = fy_get(decider_configured(cfg), name, fy_invalid);
	if (!fy_is_mapping(spec))
		spec = fy_get(fy_get(decider_canned(cfg), "deciders",
				     fy_map_empty), name, fy_invalid);
	/* A configured decider that uses a canned one of the same name. */
	use = fy_get(spec, "use", fy_invalid);
	if (fy_is_mapping(spec) && fy_equal(use, name)) {
		base = fy_get(fy_get(decider_canned(cfg), "deciders",
				     fy_map_empty), name, fy_invalid);
		/* The canned decider may extend another one itself. */
		cuse = fy_get(base, "use", fy_invalid);
		if (fy_is_mapping(base) && fy_is_string(cuse) &&
		    depth < DECIDER_USE_DEPTH) {
			cbase = decider_spec(cfg, gb, cuse, depth + 1, whyp);
			if (fy_is_mapping(cbase)) {
				fy_foreach_key_value(k, v, base)
					if (!fy_equal(k, "use"))
						cbase = fy_assoc(gb, cbase, k, v);
				base = cbase;
			} else {
				base = fy_invalid;
			}
		}
	} else if (fy_is_string(use))
		base = depth < DECIDER_USE_DEPTH ?
		       decider_spec(cfg, gb, use, depth + 1, whyp) :
		       fy_invalid;
	else
		base = fy_null;
	if (!fy_is_mapping(spec)) {
		*whyp = fy_gb_intern_string(gb, fy_sprintfa(
			"no decider '%s'", fy_castp(&name, "?")));
		return fy_invalid;
	}
	if (fy_is_invalid(base)) {
		if (!*whyp)
			*whyp = fy_gb_intern_string(gb, fy_sprintfa(
				"the decider '%s' uses '%s', which cannot be "
				"resolved", fy_castp(&name, "?"),
				fy_castp(&use, "?")));
		return fy_invalid;
	}
	if (!fy_is_mapping(base))
		return spec;
	fy_foreach_key_value(k, v, spec)
		if (!fy_equal(k, "use"))
			base = fy_assoc(gb, base, k, v);
	return base;
}

fy_generic fyai_decider_get(struct fyai_cfg *cfg, struct fy_generic_builder *gb,
			    const char *name, const char **whyp)
{
	fy_generic spec, options, gname = fy_value(gb, name);

	*whyp = NULL;
	spec = decider_spec(cfg, gb, gname, 0, whyp);
	if (fy_is_invalid(spec))
		return fy_invalid;
	options = fyai_decider_options(cfg, gb,
			fy_get(spec, "options", fy_invalid), whyp);
	if (fy_is_invalid(options))
		return fy_invalid;
	return fy_assoc(gb, spec, "name", gname, "options", options);
}

fy_generic fyai_deciders_rows(struct fyai_cfg *cfg,
			      struct fy_generic_builder *gb)
{
	fy_generic rows = fy_seq_empty, name, spec, d, keys, k, v, seen;
	fy_generic sources[2];
	const char *why;
	size_t i;

	sources[0] = decider_configured(cfg);
	sources[1] = fy_get(decider_canned(cfg), "deciders", fy_map_empty);
	seen = fy_map_empty;
	for (i = 0; i < 2; i++) {
		fy_foreach_key_value(name, spec, sources[i]) {
			if (fy_is_valid(fy_get(seen, name, fy_invalid)))
				continue;
			seen = fy_assoc(gb, seen, name, true);
			why = NULL;
			d = fyai_decider_get(cfg, gb, fy_castp(&name, ""), &why);
			keys = fy_seq_empty;
			fy_foreach_key_value(k, v, fy_get(d, "options",
							  fy_map_empty))
				keys = fy_append(gb, keys, k);
			rows = fy_append(gb, rows, fy_mapping(gb,
				"name", name,
				"from", i ? "canned" : "configuration",
				"options", keys,
				"description", fy_is_valid(d) ?
					fy_get(d, "description", fy_null) :
					fy_value(gb, why ? why : "invalid")));
		}
	}
	return rows;
}

fy_generic fyai_decider_image_limits(struct fyai_cfg *cfg, const char *wire)
{
	const char *base = wire ? strrchr(wire, '/') : NULL;
	fy_generic e, pat;

	base = base ? base + 1 : wire;
	if (!base)
		return fy_invalid;
	fy_foreach(e, fy_get(decider_canned(cfg), "image_limits",
			     fy_seq_empty)) {
		pat = fy_get(e, "model", fy_invalid);
		if (fy_is_string(pat) &&
		    !fnmatch(fy_castp(&pat, ""), base, 0))
			return e;
	}
	return fy_invalid;
}

/* The decisions endpoint of a catalogue provider, else its System One one. */
static fy_generic decider_catalog_endpoint(fy_generic provider)
{
	fy_generic e, systemone = fy_invalid;

	fy_foreach(e, fy_get(provider, "endpoints", fy_seq_empty)) {
		if (fy_equal(fy_get(e, "protocol", fy_invalid), "decisions"))
			return e;
		if (fy_equal(fy_get(e, "protocol", fy_invalid), "systemone"))
			systemone = e;
	}
	return systemone;
}

/*
 * A decision model of the catalogue: PROVIDER/MODEL pins the provider, else
 * the first provider that offers MODEL. The model must have the capability
 * `decisions`, and the provider an endpoint that serves it.
 */
static fy_generic decider_catalog(struct fyai_cfg *cfg,
				  struct fy_generic_builder *gb,
				  const char *model, const char **wirep)
{
	fy_generic cat = fyai_catalog_effective(cfg->catalog, cfg->gb);
	fy_generic prov = fy_invalid, offer = fy_invalid, entry, ep, name;
	fy_generic canon, pmid;
	const char *slash = strchr(model, '/');

	if (slash) {
		prov = fyai_catalog_provider(cat, fy_sprintfa("%.*s",
					     (int)(slash - model), model));
		if (fy_is_valid(prov) &&
		    fy_is_valid(fyai_catalog_offering(prov, slash + 1, &offer)))
			model = slash + 1;
		else
			prov = fy_invalid;
	}
	if (fy_is_invalid(prov))
		prov = fyai_catalog_provider_for_model(cat, model, &offer);
	canon = fy_get(offer, "canonical_id", fy_invalid);
	entry = fyai_catalog_model(cat, fy_castp(&canon, ""));
	ep = decider_catalog_endpoint(prov);
	if (!fyai_catalog_model_has_cap(entry, "decisions") ||
	    fy_is_invalid(ep))
		return fy_invalid;
	pmid = fy_get(offer, "provider_model_id", fy_invalid);
	name = fy_get(prov, "name", fy_invalid);
	*wirep = fy_gb_intern_string(gb, fy_is_string(pmid) ?
				     fy_castp(&pmid, "") : model);
	return fy_mapping(gb,
		"name", name,
		"url", fy_stringf(gb, "%s%s", fy_get(prov, "root_url", ""),
				  fy_get(ep, "endpoint", "")),
		"key_env", fyai_config_provider_env_key(gb,
					fy_castp(&name, "")),
		"max_questions", 8);
}

fy_generic fyai_decider_systemone(struct fyai_cfg *cfg,
				  struct fy_generic_builder *gb,
				  const char *model, const char **wirep)
{
	const char *slash;
	fy_generic name, provider;

	*wirep = NULL;
	if (!model || !*model)
		return fy_invalid;
	/* A provider of the System One table first: it is named on purpose. */
	slash = strchr(model, '/');
	if (slash && slash != model && slash[1]) {
		name = fy_stringf(gb, "%.*s", (int)(slash - model), model);
		provider = fy_get(fy_get(decider_canned(cfg), "systemone",
					 fy_map_empty), name, fy_invalid);
		if (fy_is_mapping(provider)) {
			*wirep = fy_gb_intern_string(gb, slash + 1);
			return fy_assoc(gb, provider, "name", name);
		}
	}
	return decider_catalog(cfg, gb, model, wirep);
}
