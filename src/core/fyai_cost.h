/*
 * fyai_cost.h - model prices and the estimated cost of model calls
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_COST_H
#define FYAI_COST_H

#include "fyai.h"

/* The price of one offering, in USD per million tokens. */
struct fyai_pricing {
	double input;
	double output;
	double cache_read;	/* the input price when the offering has none */
	double cache_write;	/* the input price when the offering has none */
	bool explicit_cache;	/* the caller marks the prefix to cache */
};

/*
 * Find the price of @model on @provider in the catalogue of the branch.
 * @provider can be NULL, or name a provider that does not offer the model;
 * the first offering of the model is used then. Returns false when the
 * catalogue has no price for the model.
 */
bool fyai_pricing_lookup(struct fyai_cfg *cfg, const char *provider,
			 const char *model, struct fyai_pricing *price);

/*
 * The cost in USD of one call. @input counts every prompt token, @cached
 * and @cache_write are the parts of it that the cache read and wrote.
 */
double fyai_pricing_cost(const struct fyai_pricing *price, long long input,
			 long long cached, long long cache_write,
			 long long output);

/*
 * The estimated cost of the first request after a change of model, when
 * @prefix tokens of conversation go to @to with no cache: @fresh is what
 * @to charges for them, and @stay what @from would charge to read them
 * from its cache. The difference is what the change costs.
 */
struct fyai_switch_cost {
	double fresh;
	double stay;
};

void fyai_pricing_switch(const struct fyai_pricing *from,
			 const struct fyai_pricing *to, long long prefix,
			 struct fyai_switch_cost *cost);

/* Format a cost for a status row: "~$0.0123" when @estimated. */
void fyai_cost_format(char *buf, size_t size, double cost, bool estimated);

#endif /* FYAI_COST_H */
