/*
 * fyai_usage_sum.h - token and cost totals
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_USAGE_SUM_H
#define FYAI_USAGE_SUM_H

/* Token and cost totals of the usage that some turns carry. */
struct fyai_usage_sum {
	long long input;
	long long cached;
	long long cache_write;
	long long output;
	long long reasoning;
	long long total;
	double cost;
	double cost_est;	/* the part of @cost that is estimated */
	int calls;
};

#endif
