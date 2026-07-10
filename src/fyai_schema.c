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

#include <math.h>
#include <float.h>
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
			return isfinite(d) && trunc(d) == d;
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

static bool schema_values_equal(fy_generic a, fy_generic b)
{
	fy_generic key, av, bv;
	long double da, db;
	size_t i, n;

	if (schema_is_numeric(a) && schema_is_numeric(b)) {
		da = fy_generic_is_int(a) ? (long double)fy_cast(a, 0LL) :
			(long double)fy_cast(a, 0.0);
		db = fy_generic_is_int(b) ? (long double)fy_cast(b, 0LL) :
			(long double)fy_cast(b, 0.0);
		return da == db;
	}
	if (fy_generic_is_sequence(a) && fy_generic_is_sequence(b)) {
		n = fy_len(a);
		if (n != fy_len(b))
			return false;
		for (i = 0; i < n; i++) {
			if (!schema_values_equal(fy_get_at(a, i), fy_get_at(b, i)))
				return false;
		}
		return true;
	}
	if (fy_generic_is_mapping(a) && fy_generic_is_mapping(b)) {
		n = fy_generic_mapping_get_pair_count(a);
		if (n != fy_generic_mapping_get_pair_count(b))
			return false;
		fy_foreach(key, a) {
			av = fy_get(a, key);
			bv = fy_get(b, key);
			if (fy_generic_is_invalid(bv) ||
			    !schema_values_equal(av, bv))
				return false;
		}
		return true;
	}
	return fy_equal(a, b);
}

static bool schema_is_multiple_of(fy_generic instance, fy_generic divisor)
{
	double d, multiple, quotient, nearest, tolerance;
	long long ivalue, idivisor;

	if (!schema_is_numeric(instance) || !schema_is_numeric(divisor))
		return true;

	if (fy_generic_is_int(instance) && fy_generic_is_int(divisor)) {
		ivalue = fy_cast(instance, 0LL);
		idivisor = fy_cast(divisor, 0LL);
		return idivisor > 0 && ivalue % idivisor == 0;
	}

	d = schema_to_double(instance);
	multiple = schema_to_double(divisor);
	if (!isfinite(d) || !isfinite(multiple) || multiple <= 0.0)
		return false;
	quotient = d / multiple;
	nearest = round(quotient);
	tolerance = DBL_EPSILON * fmax(1.0, fabs(quotient)) * 8.0;
	return fabs(quotient - nearest) <= tolerance;
}

static bool schema_nonnegative_size(fy_generic v, size_t *sizep)
{
	long long value;

	if (!fy_generic_is_int(v))
		return false;
	value = fy_cast(v, 0LL);
	if (value < 0 || (unsigned long long)value > SIZE_MAX)
		return false;
	*sizep = (size_t)value;
	return true;
}

/*
 * JSON strings are valid UTF-8, so each non-continuation byte starts a code
 * point.
 */
static size_t schema_utf8_length(const char *s)
{
	size_t len = 0;

	for (; *s; s++) {
		if (((unsigned char)*s & 0xc0) != 0x80)
			len++;
	}
	return len;
}

/*
 * Best-effort "uri" format check: an ASCII letter followed by zero or more
 * ASCII alphanumeric/+-. characters and ':'. Other formats are accepted.
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
			if (i == 0 && !((s[i] >= 'A' && s[i] <= 'Z') ||
					  (s[i] >= 'a' && s[i] <= 'z')))
				return false;
			if (i > 0 && !((s[i] >= 'A' && s[i] <= 'Z') ||
				       (s[i] >= 'a' && s[i] <= 'z') ||
				       (s[i] >= '0' && s[i] <= '9') ||
				       s[i] == '+' || s[i] == '-' || s[i] == '.'))
				return false;
		}
		return false;
	}
	/* Unknown format: accept (we only promised uri). */
	return true;
}

static int schema_regex_matches(const char *pattern, const char *value)
{
	regex_t re;
	int rc;

	rc = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
	if (rc)
		return -1;
	rc = regexec(&re, value, 0, NULL, 0);
	regfree(&re);
	return rc == 0;
}

static fy_generic schema_resolve_pointer(fy_generic root, const char *ref)
{
	fy_generic current;
	const char *p, *end;
	char *token, *out;
	size_t len, index;

	if (!ref || ref[0] != '#')
		return fy_invalid;
	if (!ref[1])
		return root;
	if (ref[1] != '/')
		return fy_invalid;

	current = root;
	p = ref + 2;
	for (;;) {
		end = strchr(p, '/');
		len = end ? (size_t)(end - p) : strlen(p);
		token = malloc(len + 1);
		if (!token)
			return fy_invalid;
		out = token;
		while (len--) {
			if (*p != '~') {
				*out++ = *p++;
				continue;
			}
			p++;
			if (!len || (*p != '0' && *p != '1')) {
				free(token);
				return fy_invalid;
			}
			*out++ = *p++ == '0' ? '~' : '/';
			len--;
		}
		*out = '\0';

		if (fy_generic_is_mapping(current)) {
			current = fy_get(current, token);
		} else if (fy_generic_is_sequence(current)) {
			index = 0;
			if (!*token) {
				free(token);
				return fy_invalid;
			}
			for (out = token; *out; out++) {
				if (*out < '0' || *out > '9' ||
				    index > (SIZE_MAX - (*out - '0')) / 10) {
					free(token);
					return fy_invalid;
				}
				index = index * 10 + (*out - '0');
			}
			current = fy_get_at(current, index);
		} else {
			current = fy_invalid;
		}
		free(token);
		if (fy_generic_is_invalid(current) || !end)
			return current;
		p = end + 1;
	}
}

static fy_generic schema_reject_unsupported(struct fy_generic_builder *gb,
					    fy_generic schema, const char *path,
					    fy_generic problems)
{
	static const char * const keywords[] = {
		"$id", "$anchor", "$dynamicRef", "$dynamicAnchor", "$vocabulary",
		"unevaluatedProperties", "unevaluatedItems",
		/* Pre-2020-12 spellings whose semantics are not implemented. */
		"definitions", "dependencies", "additionalItems",
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(keywords); i++) {
		if (fy_generic_is_invalid(fy_get(schema, keywords[i])))
			continue;
		problems = schema_problem_add(gb, problems,
			"%s: unsupported JSON Schema keyword '%s'",
			path[0] ? path : "<root>", keywords[i]);
	}
	return problems;
}

static fy_generic schema_check_supported(struct fy_generic_builder *gb,
					 fy_generic schema, const char *path,
					 fy_generic problems,
					 unsigned int depth)
{
	static const char * const direct[] = {
		"additionalProperties", "propertyNames", "items", "contains",
		"not", "if", "then", "else",
	};
	static const char * const schema_maps[] = {
		"properties", "patternProperties", "dependentSchemas", "$defs",
	};
	static const char * const schema_sequences[] = {
		"prefixItems", "allOf", "anyOf", "oneOf",
	};
	fy_generic child, collection, key;
	const char *ref;
	char *cpath, *epath;
	size_t i, j, n;

	if (fy_generic_is_bool(schema) || fy_generic_is_invalid(schema))
		return problems;
	if (!fy_generic_is_mapping(schema))
		return schema_problem_add(gb, problems,
			"%s: schema must be a mapping or boolean", path);
	if (depth > 128)
		return schema_problem_add(gb, problems,
			"%s: schema recursion limit exceeded", path);

	problems = schema_reject_unsupported(gb, schema, path, problems);
	child = fy_get(schema, "$ref");
	if (fy_generic_is_string(child)) {
		ref = fy_castp(&child, "");
		if (ref[0] != '#')
			problems = schema_problem_add(gb, problems,
				"%s: external $ref is unsupported: '%s'", path, ref);
		else if (ref[1] && ref[1] != '/')
			problems = schema_problem_add(gb, problems,
				"%s: anchor $ref is unsupported: '%s'", path, ref);
	}

	for (i = 0; i < ARRAY_SIZE(direct); i++) {
		child = fy_get(schema, direct[i]);
		if (fy_generic_is_invalid(child))
			continue;
		cpath = schema_path_join(path, direct[i]);
		if (!cpath)
			continue;
		problems = schema_check_supported(gb, child, cpath, problems,
						  depth + 1);
		free(cpath);
	}
	for (i = 0; i < ARRAY_SIZE(schema_maps); i++) {
		collection = fy_get(schema, schema_maps[i]);
		if (!fy_generic_is_mapping(collection))
			continue;
		fy_foreach(key, collection) {
			cpath = schema_path_join(path, schema_maps[i]);
			if (!cpath)
				continue;
			epath = schema_path_join(cpath, fy_castp(&key, ""));
			free(cpath);
			if (!epath)
				continue;
			problems = schema_check_supported(gb,
				fy_get(collection, key), epath, problems, depth + 1);
			free(epath);
		}
	}
	for (i = 0; i < ARRAY_SIZE(schema_sequences); i++) {
		collection = fy_get(schema, schema_sequences[i]);
		if (!fy_generic_is_sequence(collection))
			continue;
		n = fy_len(collection);
		for (j = 0; j < n; j++) {
			cpath = schema_path_join(path, schema_sequences[i]);
			if (!cpath)
				continue;
			epath = schema_path_index(cpath, j);
			free(cpath);
			if (!epath)
				continue;
			problems = schema_check_supported(gb,
				fy_get_at(collection, j), epath, problems, depth + 1);
			free(epath);
		}
	}
	return problems;
}

/*
 * Validate @instance against @schema, accumulating problems into @problems.
 * @path is the JSON-pointer-ish path to the instance node ("" for root).
 * Returns the (possibly extended) problems sequence.
 */
static fy_generic schema_validate(struct fy_generic_builder *gb,
				 fy_generic root, fy_generic schema,
				 fy_generic instance, const char *path,
				 fy_generic problems, unsigned int depth)
{
	fy_generic type, v, sub, resolved;
	fy_generic key, val;
	fy_generic rep;
	fy_generic properties, required, addl, prop_names, pattern_props;
	fy_generic dependencies, dependency, prefix_items;
	fy_generic prop_schema;
	fy_generic pattern_key, pattern_schema;
	fy_generic elem;
	fy_generic items, contains;
	const char *s, *pat;
	char *cpath;
	regex_t re;
	bool found, property_matched;
	size_t i, j, n, len, limit, contains_count;
	double d;
	int rc, matches;

	if (fy_generic_is_invalid(schema))
		return problems;	/* no schema => accept */
	if (depth > 128)
		return schema_problem_add(gb, problems,
			"%s: schema recursion limit exceeded",
			path[0] ? path : "<root>");
	if (fy_generic_is_bool(schema)) {
		if (!fy_cast(schema, (_Bool)true))
			problems = schema_problem_add(gb, problems,
				"%s: false schema does not allow this value",
				path[0] ? path : "<root>");
		return problems;
	}

	v = fy_get(schema, "$ref");
	if (fy_generic_is_string(v)) {
		const char *ref = fy_castp(&v, "");

		if (ref[0] != '#')
			return schema_problem_add(gb, problems,
				"%s: external $ref is unsupported: '%s'",
				path[0] ? path : "<root>", ref);
		if (ref[1] && ref[1] != '/')
			return schema_problem_add(gb, problems,
				"%s: anchor $ref is unsupported: '%s'",
				path[0] ? path : "<root>", ref);
		resolved = schema_resolve_pointer(root, ref);
		if (fy_generic_is_invalid(resolved))
			problems = schema_problem_add(gb, problems,
				"%s: unresolved local $ref '%s'",
				path[0] ? path : "<root>", fy_castp(&v, ""));
		else
			problems = schema_validate(gb, root, resolved, instance,
						   path, problems, depth + 1);
	}

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
	if (fy_generic_is_valid(v) && !schema_values_equal(v, instance))
		problems = schema_problem_add(gb, problems,
			"%s: does not match const", path[0] ? path : "<root>");

	/* ---- enum ---- */
	v = fy_get(schema, "enum");
	if (fy_generic_is_valid(v) && fy_generic_is_sequence(v)) {
		if (schema_is_numeric(instance)) {
			found = false;
			fy_foreach(sub, v) {
				if (schema_values_equal(sub, instance)) {
					found = true;
					break;
				}
			}
		} else {
			found = fy_cast(fy_contains(v, instance), (_Bool)false);
		}
		if (!found)
			problems = schema_problem_add(gb, problems,
				"%s: not in enum", path[0] ? path : "<root>");
	}

	/* ---- string keywords ---- */
	if (fy_generic_is_string(instance)) {
		s = fy_castp(&instance, "");
		len = schema_utf8_length(s);

		v = fy_get(schema, "minLength");
		if (schema_nonnegative_size(v, &limit) && len < limit)
			problems = schema_problem_add(gb, problems,
				"%s: string length %zu < minLength %lld",
				path[0] ? path : "<root>", len,
				(long long)fy_cast(v, 0LL));

		v = fy_get(schema, "maxLength");
		if (schema_nonnegative_size(v, &limit) && len > limit)
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

		v = fy_get(schema, "multipleOf");
		if (schema_is_numeric(v) && !schema_is_multiple_of(instance, v))
			problems = schema_problem_add(gb, problems,
				"%s: %g is not a multiple of %g",
				path[0] ? path : "<root>", d,
				schema_to_double(v));
	}

	/* ---- object keywords ---- */
	if (fy_generic_is_mapping(instance)) {

		properties = fy_get(schema, "properties");
		n = fy_generic_mapping_get_pair_count(instance);

		v = fy_get(schema, "minProperties");
		if (schema_nonnegative_size(v, &limit) && n < limit)
			problems = schema_problem_add(gb, problems,
				"%s: property count %zu < minProperties %zu",
				path[0] ? path : "<root>", n, limit);

		v = fy_get(schema, "maxProperties");
		if (schema_nonnegative_size(v, &limit) && n > limit)
			problems = schema_problem_add(gb, problems,
				"%s: property count %zu > maxProperties %zu",
				path[0] ? path : "<root>", n, limit);

		/* required */
		required = fy_get(schema, "required");
		if (fy_generic_is_sequence(required)) {
			fy_foreach(sub, required) {
				if (fy_generic_is_invalid(fy_get(instance, sub))) {
					problems = schema_problem_add(
						gb, problems,
						"%s: missing required key '%s'",
						path[0] ? path : "<root>",
						fy_castp(&sub, ""));
				}
			}
		}

		/* dependentRequired */
		dependencies = fy_get(schema, "dependentRequired");
		if (fy_generic_is_mapping(dependencies)) {
			fy_foreach(key, dependencies) {
				if (fy_generic_is_invalid(fy_get(instance, key)))
					continue;
				dependency = fy_get(dependencies, key);
				if (!fy_generic_is_sequence(dependency))
					continue;
				fy_foreach(sub, dependency) {
					if (fy_generic_is_valid(fy_get(instance, sub)))
						continue;
					problems = schema_problem_add(gb, problems,
						"%s: key '%s' requires key '%s'",
						path[0] ? path : "<root>",
						fy_castp(&key, ""), fy_castp(&sub, ""));
				}
			}
		}

		/* dependentSchemas */
		dependencies = fy_get(schema, "dependentSchemas");
		if (fy_generic_is_mapping(dependencies)) {
			fy_foreach(key, dependencies) {
				if (fy_generic_is_invalid(fy_get(instance, key)))
					continue;
				dependency = fy_get(dependencies, key);
				problems = schema_validate(gb, root, dependency,
					instance, path, problems, depth + 1);
			}
		}

		/* propertyNames */
		prop_names = fy_get(schema, "propertyNames");
		if (fy_generic_is_mapping(prop_names) ||
		    fy_generic_is_bool(prop_names)) {
			fy_foreach(key, instance) {
				rep = schema_validate(gb, root, prop_names, key,
					"<key>", fy_seq_empty, depth + 1);
				if (schema_problem_any(rep)) {
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
		pattern_props = fy_get(schema, "patternProperties");

		fy_foreach(key, instance) {
			val = fy_get(instance, key);
			cpath = schema_path_join(path, fy_castp(&key, ""));
			if (!cpath) {
				problems = schema_problem_add(gb, problems,
					"%s: unable to construct property path",
					path[0] ? path : "<root>");
				continue;
			}

			prop_schema = fy_generic_is_mapping(properties) ?
				fy_get(properties, key) : fy_invalid;
			property_matched = fy_generic_is_valid(prop_schema);

			if (fy_generic_is_valid(prop_schema)) {
				problems = schema_validate(gb, root, prop_schema,
					val, cpath, problems, depth + 1);
			}
			if (fy_generic_is_mapping(pattern_props)) {
				fy_foreach(pattern_key, pattern_props) {
					rc = schema_regex_matches(
						fy_castp(&pattern_key, ""),
						fy_castp(&key, ""));
					if (rc < 0) {
						problems = schema_problem_add(gb, problems,
							"%s: invalid patternProperties pattern '%s'",
							path[0] ? path : "<root>",
							fy_castp(&pattern_key, ""));
						continue;
					}
					if (!rc)
						continue;
					property_matched = true;
					pattern_schema = fy_get(pattern_props,
								pattern_key);
					problems = schema_validate(gb, root,
						pattern_schema, val, cpath, problems,
						depth + 1);
				}
			}

			if (!property_matched && fy_generic_is_bool(addl)) {
				/* additionalProperties: false */
				if (!fy_cast(addl, (_Bool)true)) {
					problems = schema_problem_add(gb,
						problems,
						"%s: additional property '%s' not allowed",
						path[0] ? path : "<root>",
						fy_castp(&key, ""));
				}
			} else if (!property_matched &&
				   (fy_generic_is_mapping(addl) ||
				    fy_generic_is_bool(addl))) {
				/* additionalProperties as schema */
				problems = schema_validate(gb, root, addl, val,
					cpath, problems, depth + 1);
			}
			free(cpath);
		}
	}

	/* ---- array keywords ---- */
	if (fy_generic_is_sequence(instance)) {
		prefix_items = fy_get(schema, "prefixItems");
		n = fy_len(instance);
		if (fy_generic_is_sequence(prefix_items)) {
			limit = fy_len(prefix_items);
			if (limit > n)
				limit = n;
			for (i = 0; i < limit; i++) {
				cpath = schema_path_index(path, i);
				if (!cpath) {
					problems = schema_problem_add(gb, problems,
						"%s: unable to construct item path",
						path[0] ? path : "<root>");
					continue;
				}
				problems = schema_validate(gb, root,
					fy_get_at(prefix_items, i),
					fy_get_at(instance, i), cpath, problems,
					depth + 1);
				free(cpath);
			}
		}

		items = fy_get(schema, "items");
		if (fy_generic_is_mapping(items) || fy_generic_is_bool(items)) {
			i = fy_generic_is_sequence(prefix_items) ?
				fy_len(prefix_items) : 0;
			for (; i < n; i++) {
				elem = fy_get_at(instance, i);
				cpath = schema_path_index(path, i);
				if (!cpath) {
					problems = schema_problem_add(gb, problems,
						"%s: unable to construct item path",
						path[0] ? path : "<root>");
					continue;
				}
				problems = schema_validate(gb, root, items, elem,
					cpath, problems, depth + 1);
				free(cpath);
			}
		}

		v = fy_get(schema, "minItems");
		if (schema_nonnegative_size(v, &limit) && fy_len(instance) < limit)
			problems = schema_problem_add(gb, problems,
				"%s: array length %zu < minItems %lld",
				path[0] ? path : "<root>",
				(size_t)fy_len(instance),
				(long long)fy_cast(v, 0LL));

		v = fy_get(schema, "maxItems");
		if (schema_nonnegative_size(v, &limit) && fy_len(instance) > limit)
			problems = schema_problem_add(gb, problems,
				"%s: array length %zu > maxItems %lld",
				path[0] ? path : "<root>",
				(size_t)fy_len(instance),
				(long long)fy_cast(v, 0LL));

		v = fy_get(schema, "uniqueItems");
		if (fy_generic_is_bool(v) && fy_cast(v, (_Bool)false)) {
			n = fy_len(instance);
			for (i = 0; i < n; i++) {
				for (j = i + 1; j < n; j++) {
					if (!schema_values_equal(fy_get_at(instance, i),
							 fy_get_at(instance, j)))
						continue;
					problems = schema_problem_add(gb, problems,
						"%s: array items %zu and %zu are not unique",
						path[0] ? path : "<root>", i, j);
					goto unique_done;
				}
			}
		}
unique_done:

		contains = fy_get(schema, "contains");
		if (fy_generic_is_mapping(contains) ||
		    fy_generic_is_bool(contains)) {
			contains_count = 0;
			n = fy_len(instance);
			for (i = 0; i < n; i++) {
				rep = schema_validate(gb, root, contains,
					fy_get_at(instance, i), path, fy_seq_empty,
					depth + 1);
				if (!schema_problem_any(rep))
					contains_count++;
			}

			limit = 1;
			v = fy_get(schema, "minContains");
			(void)schema_nonnegative_size(v, &limit);
			if (contains_count < limit)
				problems = schema_problem_add(gb, problems,
					"%s: contains matched %zu items, fewer than %zu",
					path[0] ? path : "<root>",
					contains_count, limit);

			v = fy_get(schema, "maxContains");
			if (schema_nonnegative_size(v, &limit) &&
			    contains_count > limit)
				problems = schema_problem_add(gb, problems,
					"%s: contains matched %zu items, more than %zu",
					path[0] ? path : "<root>",
					contains_count, limit);
		}
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
			rep = schema_validate(gb, root, sub, instance, path,
					      fy_seq_empty, depth + 1);
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
			rep = schema_validate(gb, root, sub, instance, path,
					      fy_seq_empty, depth + 1);
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
			rep = schema_validate(gb, root, sub, instance, path,
					      fy_seq_empty, depth + 1);
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
	if (fy_generic_is_mapping(v) || fy_generic_is_bool(v)) {
		rep = schema_validate(gb, root, v, instance, path, fy_seq_empty,
				      depth + 1);
		if (!schema_problem_any(rep))
			problems = schema_problem_add(gb, problems,
				"%s: 'not' subschema matched (should not)",
				path[0] ? path : "<root>");
	}

	/* if/then/else: select a branch using the result of the if schema. */
	v = fy_get(schema, "if");
	if (fy_generic_is_mapping(v) || fy_generic_is_bool(v)) {
		rep = schema_validate(gb, root, v, instance, path, fy_seq_empty,
				      depth + 1);
		sub = schema_problem_any(rep) ? fy_get(schema, "else") :
			fy_get(schema, "then");
		if (fy_generic_is_mapping(sub) || fy_generic_is_bool(sub))
			problems = schema_validate(gb, root, sub, instance, path,
				problems, depth + 1);
	}

	return problems;
}

fy_generic fyai_schema_validate(struct fy_generic_builder *gb,
			       fy_generic schema, fy_generic instance)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_CREATE_ALLOCATOR | FYGBCF_SCOPE_LEADER |
			 FYGBCF_DEDUP_ENABLED,
		.parent = gb,
	};
	struct fy_generic_builder *validation_gb;
	fy_generic problems, report, out;

	validation_gb = fy_generic_builder_create(&gb_cfg);
	if (!validation_gb)
		return fy_mapping(gb, "result", "failed", "problems",
			fy_sequence(gb,
				"<schema>: unable to create validation builder"));

	if (fy_generic_is_invalid(schema)) {
		report = fy_mapping(validation_gb, "result", "ok");
		goto out;
	}

	problems = schema_check_supported(validation_gb, schema, "<schema>",
					  fy_seq_empty, 0);
	if (schema_problem_any(problems)) {
		report = fy_mapping(validation_gb, "result", "failed",
				       "problems", problems);
		goto out;
	}

	problems = schema_validate(validation_gb, schema, schema, instance, "",
				   fy_seq_empty, 0);
	if (schema_problem_any(problems))
		report = fy_mapping(validation_gb, "result", "failed",
				       "problems", problems);
	else
		report = fy_mapping(validation_gb, "result", "ok");

out:
	out = fy_gb_internalize(gb, report);
	fy_generic_builder_destroy(validation_gb);
	return out;
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
