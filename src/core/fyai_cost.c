/*
 * fyai_cost.c - model prices and the estimated cost of model calls
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include "fyai.h"
#include "fyai_catalog.h"
#include "fyai_cost.h"

#define FYAI_MODULE FYAIEM_CATALOG

static double price_number(fy_generic v, double dflt)
{
	return fy_is_valid(v) ? fy_number(v, dflt) : dflt;
}

/* The model id without a leading name of a catalogue provider. */
static const char *bare_model(fy_generic catalog, const char *model)
{
	const char *slash = strchr(model, '/');
	char name[64];
	size_t len;

	if (!slash)
		return model;
	len = (size_t)(slash - model);
	if (len >= sizeof(name))
		return model;
	memcpy(name, model, len);
	name[len] = '\0';
	return fy_is_valid(fyai_catalog_provider(catalog, name)) ?
	       slash + 1 : model;
}

bool fyai_pricing_lookup(struct fyai_cfg *cfg, const char *provider,
			 const char *model, struct fyai_pricing *price)
{
	fy_generic catalog, prov, offer, pricing, extra, v;
	double in, out, cr, cw;

	if (!cfg || !model || !*model)
		return false;
	catalog = fyai_catalog_effective(cfg->catalog, cfg->gb);
	if (fy_is_invalid(catalog))
		return false;
	model = bare_model(catalog, model);
	offer = fy_invalid;
	prov = provider ? fyai_catalog_provider(catalog, provider) : fy_invalid;
	if (fy_is_valid(prov))
		(void)fyai_catalog_offering(prov, model, &offer);
	if (fy_is_invalid(offer))
		prov = fyai_catalog_provider_for_model(catalog, model, &offer);
	if (fy_is_invalid(offer))
		return false;
	pricing = fy_get(offer, "pricing", fy_invalid);
	in = price_number(fy_get(pricing, "input", fy_invalid), -1.0);
	out = price_number(fy_get(pricing, "output", fy_invalid), -1.0);
	if (in < 0.0 || out < 0.0)
		return false;
	extra = fy_get(pricing, "extra", fy_invalid);
	cr = price_number(fy_get(extra, "cache_read", fy_invalid), in);
	v = fy_get(extra, "cache_write", fy_invalid);
	if (fy_is_invalid(v))
		v = fy_get(extra, "cache_write_5m", fy_invalid);
	cw = price_number(v, in);
	price->input = in;
	price->output = out;
	price->cache_read = cr;
	price->cache_write = cw;
	price->explicit_cache = fy_get(fy_get(fyai_catalog_endpoint(prov,
						cfg->api_mode), "capabilities"),
				       "explicit_prompt_caching_supported",
				       (_Bool)false);
	return true;
}

double fyai_pricing_cost(const struct fyai_pricing *price, long long input,
			 long long cached, long long cache_write,
			 long long output)
{
	long long fresh = input - cached - cache_write;

	if (fresh < 0)
		fresh = 0;
	return ((double)fresh * price->input +
		(double)cached * price->cache_read +
		(double)cache_write * price->cache_write +
		(double)output * price->output) / 1e6;
}

void fyai_pricing_switch(const struct fyai_pricing *from,
			 const struct fyai_pricing *to, long long prefix,
			 struct fyai_switch_cost *cost)
{
	double rate = to->explicit_cache ? to->cache_write : to->input;

	cost->fresh = (double)prefix * rate / 1e6;
	cost->stay = (double)prefix * from->cache_read / 1e6;
}

void fyai_cost_format(char *buf, size_t size, double cost, bool estimated)
{
	snprintf(buf, size, "%s$%.4f", estimated ? "~" : "", cost);
}
