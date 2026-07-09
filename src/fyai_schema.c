/*
 * fyai_schema.c - rudimentary JSON Schema (draft 2020-12 subset) validator
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * Uses libfyaml generics as the data model, following the report convention
 * of fyai_config_validate_report: returns a mapping { result, problems }.
 * Good enough for parsing/validating tool schemas from the provider catalogue.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

#include "fyai_schema.h"

/*
 * Append a problem string to @problems. @fmt is a printf-style format that
 * should include the path prefix. Mirrors config_problem_add.
 */
static fy_generic schema_problem_add(struct fy_generic_builder *gb,
				     fy_generic problems,
				     const char *fmt, ...)
{
	va_list ap;
	char *msg = NULL;
	fy_generic out;

	va_start(ap, fmt);
	if (vasprintf(&msg, fmt, ap) < 0)
		msg = NULL;
	va_end(ap);
	if (!msg)
		return problems;
	out = fy_append(gb, problems, msg);
	free(msg);
	return out;
}

static bool schema_problem_any(fy_generic problems)
{
	fy_generic p;

	fy_foreach(p, problems)
		return true;
	return false;
}

/*
 * Build a child path: "<parent>/<segment>". Segment may be a property name
 * or an array index (rendered as a string). Returns a malloc'd string.
 */
static char *schema_path_join(const char *parent, const char *segment)
{
	char *out;
	int n;

	if (!parent || !*parent)
		return strdup(segment);
	n = asprintf(&out, "%s/%s", parent, segment);
	return n < 0 ? NULL : out;
}

static char *schema_path_index(const char *parent, size_t idx)
{
	char *out;
	int n;

	if (!parent || !*parent)
		n = asprintf(&out, "%zu", idx);
	else
		n = asprintf(&out, "%s/%zu", parent, idx);
	return n < 0 ? NULL : out;
}

/*
 * Check whether @instance matches a single JSON Schema type name.
 * Non-standard types (custom, web_search, image_generation, etc.) accept
 * anything — they are opaque pass-through schemas.
 */
static bool schema_type_matches(fy_generic type_name, fy_generic instance)
{
	if (!fy_generic_is_valid(type_name))
		return true;

	if (fy_equal(type_name, "object"))
		return fy_generic_is_mapping(instance);
	if (fy_equal(type_name, "array"))
		return fy_generic_is_sequence(instance);
	if (fy_equal(type_name, "string"))
		return fy_generic_is_string(instance);
	if (fy_equal(type_name, "integer")) {
		if (fy_generic_is_int(instance))
			return true;
		/* Accept a float with zero fractional part. */
		if (fy_generic_is_float(instance)) {
			double d = fy_cast(instance, 0.0);
			return d == (double)(long long)d;
		}
		return false;
	}
	if (fy_equal(type_name, "number"))
		return fy_generic_is_int(instance) ||
		       fy_generic_is_float(instance);
	if (fy_equal(type_name, "boolean"))
		return fy_generic_is_bool(instance);
	if (fy_equal(type_name, "null"))
		return fy_generic_is_null_type(instance);

	/* Non-standard type: accept anything. */
	return true;
}

/*
 * Check the "type" keyword. @type may be a string or an array of strings.
 * Returns true if the instance matches any of the named types.
 */
static bool schema_check_type(fy_generic type, fy_generic instance)
{
	fy_generic t;

	/* Array of types: try each. */
	if (fy_generic_is_sequence(type)) {
		fy_foreach(t, type) {
			if (schema_type_matches(t, instance))
				return true;
		}
		return false;
	}

	/* Single string type. */
	if (fy_generic_is_string(type))
		return schema_type_matches(type, instance);

	/* Unknown shape: accept. */
	return true;
}

/* ---- numeric helpers (JSON numbers arrive as int or float generics) ---- */

static double schema_to_double(fy_generic v)
{
	if (fy_generic_is_int(v))
		return (double)fy_cast(v, 0LL);
	if (fy_generic_is_float(v))
		return fy_cast(v, 0.0);
	return 0.0;
}

static bool schema_is_numeric(fy_generic v)
{
	return fy_generic_is_int(v) || fy_generic_is_float(v);
}

/*
 * Best-effort "uri" format check: a non-empty scheme prefix (alphanum/+-.)
 * followed by ':'. Other format values are silently accepted.
 */
static bool schema_check_format(fy_generic fmt, fy_generic instance)
{
	const char *s;
	size_t i;

	if (!fy_generic_is_valid(fmt) || fy_equal(fmt, "uri")) {
		s = fy_cast(instance, "");
		if (!*s)
			return false;
		for (i = 0; s[i]; i++) {
			if (s[i] == ':')
				return i > 0;
			if (!(isalnum((unsigned char)s[i]) ||
			      s[i] == '+' || s[i] == '-' || s[i] == '.'))
				return false;
		}
		return false;
	}
	/* Unknown format: accept (we only promised uri). */
	return true;
}

/*
 * Validate @instance against @schema, accumulating problems into @problems.
 * @path is the JSON-pointer-ish path to the instance node ("" for root).
 * Returns the (possibly extended) problems sequence.
 */
static fy_generic schema_validate(struct fy_generic_builder *gb,
				 fy_generic schema, fy_generic instance,
				 const char *path, fy_generic problems)
{
	fy_generic type, v, sub;
	fy_generic key, val;
	fy_generic rep;
	fy_generic properties, required, addl, prop_names;
	fy_generic prop_schema;
	fy_generic elem;
	fy_generic items;
	const char *rkey, *s, *pat;
	char *cpath;
	regex_t re;
	bool found;
	size_t i, n, len;
	double d;
	int rc, matches;

	if (fy_generic_is_invalid(schema))
		return problems;	/* no schema => accept */

	/* ---- type ---- */
	type = fy_get(schema, "type");
	if (fy_generic_is_valid(type) &&
	    !schema_check_type(type, instance)) {
		problems = schema_problem_add(gb, problems,
			"%s: type mismatch", path[0] ? path : "<root>");
		/*
		 * Keep going: other keywords may add more context, but a
		 * type mismatch usually makes further checks meaningless,
		 * so return early.
		 */
		return problems;
	}

	/* ---- const ---- */
	v = fy_get(schema, "const");
	if (fy_generic_is_valid(v) && !fy_equal(v, instance))
		problems = schema_problem_add(gb, problems,
			"%s: does not match const", path[0] ? path : "<root>");

	/* ---- enum ---- */
	v = fy_get(schema, "enum");
	if (fy_generic_is_valid(v) && fy_generic_is_sequence(v)) {
#if 0
		found = false;
		fy_foreach(sub, v) {
			if (fy_equal(sub, instance)) {
				found = true;
				break;
			}
		}
#else
		found = fy_cast(fy_contains(v, instance), (_Bool)false);
#endif
		if (!found)
			problems = schema_problem_add(gb, problems,
				"%s: not in enum", path[0] ? path : "<root>");
	}

	/* ---- string keywords ---- */
	if (fy_generic_is_string(instance)) {
		s = fy_castp(&instance, "");
		len = strlen(s);

		v = fy_get(schema, "minLength");
		if (schema_is_numeric(v) && len < (size_t)fy_cast(v, 0LL))
			problems = schema_problem_add(gb, problems,
				"%s: string length %zu < minLength %lld",
				path[0] ? path : "<root>", len,
				(long long)fy_cast(v, 0LL));

		v = fy_get(schema, "maxLength");
		if (schema_is_numeric(v) && len > (size_t)fy_cast(v, 0LL))
			problems = schema_problem_add(gb, problems,
				"%s: string length %zu > maxLength %lld",
				path[0] ? path : "<root>", len,
				(long long)fy_cast(v, 0LL));

		v = fy_get(schema, "pattern");
		if (fy_generic_is_string(v)) {
			pat = fy_castp(&v, "");

			rc = regcomp(&re, pat, REG_EXTENDED | REG_NOSUB);
			if (rc) {
				problems = schema_problem_add(gb, problems,
					"%s: invalid pattern '%s'",
					path[0] ? path : "<root>", pat);
			} else {
				rc = regexec(&re, s, 0, NULL, 0);
				regfree(&re);
				if (rc == REG_NOMATCH)
					problems = schema_problem_add(gb,
						problems,
						"%s: does not match pattern '%s'",
						path[0] ? path : "<root>",
						pat);
			}
		}

		v = fy_get(schema, "format");
		if (fy_generic_is_string(v) &&
		    !schema_check_format(v, instance))
			problems = schema_problem_add(gb, problems,
				"%s: invalid format '%s'",
				path[0] ? path : "<root>",
				fy_castp(&v, ""));
	}

	/* ---- numeric keywords ---- */
	if (schema_is_numeric(instance)) {
		d = schema_to_double(instance);

		v = fy_get(schema, "minimum");
		if (schema_is_numeric(v) && d < schema_to_double(v))
			problems = schema_problem_add(gb, problems,
				"%s: %g < minimum %g",
				path[0] ? path : "<root>", d,
				schema_to_double(v));

		v = fy_get(schema, "maximum");
		if (schema_is_numeric(v) && d > schema_to_double(v))
			problems = schema_problem_add(gb, problems,
				"%s: %g > maximum %g",
				path[0] ? path : "<root>", d,
				schema_to_double(v));

		v = fy_get(schema, "exclusiveMinimum");
		if (schema_is_numeric(v) && d <= schema_to_double(v))
			problems = schema_problem_add(gb, problems,
				"%s: %g <= exclusiveMinimum %g",
				path[0] ? path : "<root>", d,
				schema_to_double(v));

		v = fy_get(schema, "exclusiveMaximum");
		if (schema_is_numeric(v) && d >= schema_to_double(v))
			problems = schema_problem_add(gb, problems,
				"%s: %g >= exclusiveMaximum %g",
				path[0] ? path : "<root>", d,
				schema_to_double(v));
	}

	/* ---- object keywords ---- */
	if (fy_generic_is_mapping(instance)) {

		properties = fy_get(schema, "properties");

		/* required */
		required = fy_get(schema, "required");
		if (fy_generic_is_sequence(required)) {
			fy_foreach(sub, required) {
				rkey = fy_castp(&sub, "");
				if (fy_generic_is_invalid(fy_get(instance, rkey))) {
					cpath = schema_path_join(path, rkey);
					problems = schema_problem_add(
						gb, problems,
						"%s: missing required key '%s'",
						path[0] ? path : "<root>",
						rkey);
					free(cpath);
				}
			}
		}

		/* propertyNames */
		prop_names = fy_get(schema, "propertyNames");
		if (fy_generic_is_mapping(prop_names)) {
			n = fy_generic_mapping_get_pair_count(instance);

			for (i = 0; i < n; i++) {
				key = fy_generic_mapping_get_at_key(instance, i);

				rep = schema_validate(gb, prop_names, key,
						      "<key>", fy_seq_empty);
				if (schema_problem_any(rep)) {
					val = fy_generic_mapping_get_at_value(
						instance, i);
					(void)val;
					problems = schema_problem_add(gb,
						problems,
						"%s: key '%s' fails propertyNames",
						path[0] ? path : "<root>",
						fy_castp(&key, ""));
				}
			}
		}

		/* properties + additionalProperties */
		addl = fy_get(schema, "additionalProperties");

		n = fy_generic_mapping_get_pair_count(instance);
		for (i = 0; i < n; i++) {
			key = fy_generic_mapping_get_at_key(instance, i);
			val = fy_generic_mapping_get_at_value(instance, i);

			cpath = schema_path_join(path, fy_castp(&key, ""));

			prop_schema = fy_generic_is_mapping(properties) ?
				fy_get(properties, key) : fy_invalid;

			if (fy_generic_is_valid(prop_schema)) {
				problems = schema_validate(gb, prop_schema,
							   val, cpath,
							   problems);
			} else if (fy_generic_is_bool(addl)) {
				/* additionalProperties: false */
				if (!fy_cast(addl, (_Bool)true)) {
					problems = schema_problem_add(gb,
						problems,
						"%s: additional property '%s' not allowed",
						path[0] ? path : "<root>",
						fy_castp(&key, ""));
				}
			} else if (fy_generic_is_mapping(addl)) {
				/* additionalProperties as schema */
				problems = schema_validate(gb, addl, val, cpath, problems);
			}
			free(cpath);
		}
	}

	/* ---- array keywords ---- */
	if (fy_generic_is_sequence(instance)) {

		items = fy_get(schema, "items");
		if (fy_generic_is_mapping(items)) {
			n = fy_len(instance);
			for (i = 0; i < n; i++) {
				elem = fy_get_at(instance, i);
				cpath = schema_path_index(path, i);
				problems = schema_validate(gb, items, elem,
							   cpath, problems);
				free(cpath);
			}
		}

		v = fy_get(schema, "minItems");
		if (schema_is_numeric(v) && fy_len(instance) < (size_t)fy_cast(v, 0LL))
			problems = schema_problem_add(gb, problems,
				"%s: array length %zu < minItems %lld",
				path[0] ? path : "<root>",
				(size_t)fy_len(instance),
				(long long)fy_cast(v, 0LL));

		v = fy_get(schema, "maxItems");
		if (schema_is_numeric(v) && fy_len(instance) > (size_t)fy_cast(v, 0LL))
			problems = schema_problem_add(gb, problems,
				"%s: array length %zu > maxItems %lld",
				path[0] ? path : "<root>",
				(size_t)fy_len(instance),
				(long long)fy_cast(v, 0LL));
	}

	/*
	 * ---- combining keywords ----
	 *
	 * anyOf/allOf/oneOf take sequences of schemas; not takes a single
	 * schema. For these we only need pass/fail, so we run a
	 * throwaway validation (empty problems) and check the result.
	 */

	/* anyOf: at least one subschema must pass. */
	v = fy_get(schema, "anyOf");
	if (fy_generic_is_sequence(v)) {
		matches = 0;
		fy_foreach(sub, v) {
			rep = schema_validate(gb, sub, instance, path, fy_seq_empty);
			if (!schema_problem_any(rep)) {
				matches++;
				break;
			}
		}
		if (!matches)
			problems = schema_problem_add(gb, problems,
				"%s: no anyOf branch matched",
				path[0] ? path : "<root>");
	}

	/* allOf: all subschemas must pass. */
	v = fy_get(schema, "allOf");
	if (fy_generic_is_sequence(v)) {
		matches = 0;
		fy_foreach(sub, v) {
			rep = schema_validate(gb, sub, instance, path, fy_seq_empty);
			if (schema_problem_any(rep))
				break;
			matches++;
		}
		if (matches != (int)fy_len(v))
			problems = schema_problem_add(gb, problems,
				"%s: allOf branch failed",
				path[0] ? path : "<root>");
	}

	/* oneOf: exactly one subschema must pass. */
	v = fy_get(schema, "oneOf");
	if (fy_generic_is_sequence(v)) {
		matches = 0;
		fy_foreach(sub, v) {
			rep = schema_validate(gb, sub, instance, path, fy_seq_empty);
			if (!schema_problem_any(rep))
				matches++;
		}
		if (matches != 1)
			problems = schema_problem_add(gb, problems,
				"%s: oneOf matched %d branches (expected 1)",
				path[0] ? path : "<root>", matches);
	}

	/* not: the subschema must fail. */
	v = fy_get(schema, "not");
	if (fy_generic_is_mapping(v)) {
		rep = schema_validate(gb, v, instance, path, fy_seq_empty);
		if (!schema_problem_any(rep))
			problems = schema_problem_add(gb, problems,
				"%s: 'not' subschema matched (should not)",
				path[0] ? path : "<root>");
	}

	return problems;
}

fy_generic fyai_schema_validate(struct fy_generic_builder *gb,
			       fy_generic schema, fy_generic instance)
{
	fy_generic problems;

	if (fy_generic_is_invalid(schema))
		return fy_gb_internalize(gb, fy_mapping("result", "ok"));

	problems = schema_validate(gb, schema, instance, "", fy_seq_empty);
	if (schema_problem_any(problems))
		return fy_gb_internalize(gb,
			fy_mapping("result", "failed", "problems", problems));

	return fy_gb_internalize(gb, fy_mapping("result", "ok"));
}

void fyai_schema_report_print(fy_generic report)
{
	fy_generic problems, p;

	if (fyai_schema_valid(report))
		return;
	problems = fy_get(report, "problems", fy_seq_empty);
	fy_foreach(p, problems)
		fprintf(stderr, "schema: %s\n", fy_castp(&p, ""));
}
