/* SPDX-License-Identifier: MIT */
#ifndef FYAI_DECIDER_H
#define FYAI_DECIDER_H

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_cfg;

/*
 * Deciders: the instructions and the options of a decision of a model. The
 * canned ones are data/decisions.yaml; `decisions/NAME` of the
 * configuration adds or replaces one. A decider may extend another with
 * `use`, list canned options by name, or take a canned option with
 * {use: NAME} and change its description or schema.
 */

/*
 * The decider @name resolved, in @gb: {name, description, instructions,
 * model, options: {NAME: {description, schema}}}, or fy_invalid with the
 * reason in @whyp (in @gb).
 */
fy_generic fyai_decider_get(struct fyai_cfg *cfg, struct fy_generic_builder *gb,
			    const char *name, const char **whyp);

/*
 * Resolve the options @options of a decider - a list of canned names or a
 * mapping of name to a canned name, {use: NAME, ...}, or a definition - to
 * {NAME: {description, schema}}, or fy_invalid with the reason in @whyp.
 */
fy_generic fyai_decider_options(struct fyai_cfg *cfg,
				struct fy_generic_builder *gb,
				fy_generic options, const char **whyp);

/* Every decider, canned and configured: {name, description, options, from}. */
fy_generic fyai_deciders_rows(struct fyai_cfg *cfg,
			      struct fy_generic_builder *gb);

#endif
