/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SCHEMA_H
#define FYAI_SCHEMA_H

#include "fyai.h"

/*
 * Validate @instance against @schema (a JSON Schema draft 2020-12 subset),
 * using libfyaml generics as the data model. Returns a report mapping in the
 * same convention as fyai_config_validate_report:
 *
 *   { result: "ok" }  on success, or
 *   { result: "failed", problems: [ "path: message", ... ] }  on failure.
 *
 * Problems carry a path prefix (e.g. "questions/0/options/1/label: expected
 * string") so the caller can point at the failing node. All strings are
 * interned in @gb.
 */
fy_generic fyai_schema_validate(struct fy_generic_builder *gb,
				fy_generic schema,
				fy_generic instance);

/* True iff the report's result is "ok". */
static inline bool fyai_schema_valid(fy_generic report)
{
	return fy_equal(fy_get(report, "result"), "ok");
}

/* Print a report's problems to stderr (no-op when ok). */
void fyai_schema_report_print(fy_generic report);

#endif
