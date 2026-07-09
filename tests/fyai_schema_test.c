/*
 * fyai_schema_test.c - unit tests for the JSON Schema validator
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_schema.h"
#include "utils.h"

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx;

static fy_generic parse(const char *json)
{
	fy_generic v;

	v = parse_json_string(test_ctx.transient_gb, json);
	if (fy_generic_is_invalid(v)) {
		fprintf(stderr, "failed to parse fixture: %s\n", json);
		exit(1);
	}
	return v;
}

static fy_generic schema(const char *json)
{
	return parse(json);
}

static fy_generic instance(const char *json)
{
	return parse(json);
}

static fy_generic validate(fy_generic sch, fy_generic inst)
{
	return fyai_schema_validate(test_ctx.transient_gb, sch, inst);
}

static void must_pass(const char *label, fy_generic sch, fy_generic inst)
{
	fy_generic report = validate(sch, inst);
	if (!fyai_schema_valid(report)) {
		fprintf(stderr, "FAIL %s: expected valid\n", label);
		fyai_schema_report_print(report);
		exit(1);
	}
}

static void must_fail(const char *label, fy_generic sch, fy_generic inst)
{
	fy_generic report = validate(sch, inst);
	if (fyai_schema_valid(report)) {
		fprintf(stderr, "FAIL %s: expected invalid\n", label);
		exit(1);
	}
}

/* must fail and the problem string must contain @substr */
static void must_fail_containing(const char *label, fy_generic sch,
				 fy_generic inst, const char *substr)
{
	fy_generic report = validate(sch, inst);
	fy_generic problems, p;

	if (fyai_schema_valid(report)) {
		fprintf(stderr, "FAIL %s: expected invalid\n", label);
		exit(1);
	}
	problems = fy_get(report, "problems", fy_seq_empty);
	fy_foreach(p, problems) {
		if (strstr(fy_castp(&p, ""), substr))
			return;
	}
	fprintf(stderr, "FAIL %s: no problem containing '%s'\n", label, substr);
	exit(1);
}

/* ---- tests ---- */

static void test_type(void)
{
	fy_generic s;

	s = schema("{\"type\":\"string\"}");
	must_pass("type/string", s, instance("\"hello\""));
	must_fail("type/string-not", s, instance("42"));

	s = schema("{\"type\":\"integer\"}");
	must_pass("type/int", s, instance("42"));
	must_fail("type/int-not", s, instance("\"x\""));

	s = schema("{\"type\":\"number\"}");
	must_pass("type/number-int", s, instance("42"));
	must_pass("type/number-float", s, instance("3.14"));
	must_fail("type/number-not", s, instance("\"x\""));

	s = schema("{\"type\":\"boolean\"}");
	must_pass("type/bool", s, instance("true"));
	must_fail("type/bool-not", s, instance("\"x\""));

	s = schema("{\"type\":\"object\"}");
	must_pass("type/object", s, instance("{\"a\":1}"));
	must_fail("type/object-not", s, instance("[1]"));

	s = schema("{\"type\":\"array\"}");
	must_pass("type/array", s, instance("[1]"));
	must_fail("type/array-not", s, instance("{\"a\":1}"));

	s = schema("{\"type\":\"null\"}");
	must_pass("type/null", s, instance("null"));
	must_fail("type/null-not", s, instance("42"));

	/* array of types */
	s = schema("{\"type\":[\"string\",\"integer\"]}");
	must_pass("type/array-str", s, instance("\"hi\""));
	must_pass("type/array-int", s, instance("42"));
	must_fail("type/array-bool", s, instance("true"));
}

static void test_required(void)
{
	fy_generic s;

	s = schema("{\"type\":\"object\","
		   "\"properties\":{\"a\":{\"type\":\"string\"}},"
		   "\"required\":[\"a\"]}");
	must_pass("required/present", s, instance("{\"a\":\"x\"}"));
	must_fail_containing("required/missing", s,
			      instance("{}"), "missing required key");
}

static void test_additional_properties_false(void)
{
	fy_generic s;

	s = schema("{\"type\":\"object\","
		   "\"properties\":{\"a\":{\"type\":\"string\"}},"
		   "\"additionalProperties\":false}");
	must_pass("addl/exact", s, instance("{\"a\":\"x\"}"));
	must_fail_containing("addl/extra", s,
			     instance("{\"a\":\"x\",\"b\":1}"),
			     "additional property");
}

static void test_additional_properties_schema(void)
{
	fy_generic s;

	s = schema("{\"type\":\"object\","
		   "\"properties\":{\"a\":{\"type\":\"string\"}},"
		   "\"additionalProperties\":{\"type\":\"integer\"}}");
	must_pass("addl-schema/valid", s,
		  instance("{\"a\":\"x\",\"b\":1}"));
	must_fail("addl-schema/invalid", s,
		  instance("{\"a\":\"x\",\"b\":\"nope\"}"));
}

static void test_enum(void)
{
	fy_generic s;

	s = schema("{\"enum\":[\"red\",\"green\",\"blue\"]}");
	must_pass("enum/in", s, instance("\"green\""));
	must_fail("enum/out", s, instance("\"purple\""));
}

static void test_const(void)
{
	fy_generic s;

	s = schema("{\"const\":42}");
	must_pass("const/equal", s, instance("42"));
	must_fail("const/unequal", s, instance("43"));
}

static void test_any_of(void)
{
	fy_generic s;

	s = schema("{\"anyOf\":[{\"type\":\"string\"},"
		   "{\"type\":\"integer\"}]}");
	must_pass("anyOf/string", s, instance("\"hi\""));
	must_pass("anyOf/int", s, instance("42"));
	must_fail("anyOf/none", s, instance("true"));
}

static void test_all_of(void)
{
	fy_generic s;

	s = schema("{\"allOf\":[{\"type\":\"object\"},"
		   "{\"required\":[\"a\"]}]}");
	must_pass("allOf/pass", s, instance("{\"a\":1}"));
	must_fail("allOf/fail", s, instance("{\"b\":1}"));
}

static void test_one_of(void)
{
	fy_generic s;

	/* exactly one matches */
	s = schema("{\"oneOf\":[{\"type\":\"string\"},"
		   "{\"type\":\"integer\"}]}");
	must_pass("oneOf/one", s, instance("\"hi\""));
	must_pass("oneOf/one-int", s, instance("42"));

	/* both match => fail */
	s = schema("{\"oneOf\":[{\"type\":\"string\"},"
		   "{\"type\":\"string\"}]}");
	must_fail_containing("oneOf/two", s, instance("\"x\""),
			     "matched 2 branches");

	/* zero match => fail */
	s = schema("{\"oneOf\":[{\"type\":\"string\"}]}");
	must_fail_containing("oneOf/zero", s, instance("true"),
			     "matched 0 branches");
}

static void test_not(void)
{
	fy_generic s;

	s = schema("{\"not\":{\"type\":\"string\"}}");
	must_fail("not/match", s, instance("\"hi\""));
	must_pass("not/nomatch", s, instance("42"));
}

static void test_string_bounds(void)
{
	fy_generic s;

	s = schema("{\"type\":\"string\",\"minLength\":2,\"maxLength\":4}");
	must_pass("len/in-range", s, instance("\"abc\""));
	must_pass("len/min", s, instance("\"ab\""));
	must_pass("len/max", s, instance("\"abcd\""));
	must_fail_containing("len/too-short", s, instance("\"a\""),
			     "minLength");
	must_fail_containing("len/too-long", s, instance("\"abcde\""),
			     "maxLength");
}

static void test_pattern(void)
{
	fy_generic s;

	s = schema("{\"type\":\"string\",\"pattern\":\"^[a-z]+$\"}");
	must_pass("pattern/match", s, instance("\"hello\""));
	must_fail_containing("pattern/miss", s, instance("\"Hello1\""),
			     "pattern");
}

static void test_numeric_bounds(void)
{
	fy_generic s;

	s = schema("{\"type\":\"number\",\"minimum\":0,\"maximum\":100}");
	must_pass("num/in-range", s, instance("50"));
	must_pass("num/min", s, instance("0"));
	must_pass("num/max", s, instance("100"));
	must_fail_containing("num/below", s, instance("-1"), "minimum");
	must_fail_containing("num/above", s, instance("101"), "maximum");

	s = schema("{\"type\":\"number\","
		   "\"exclusiveMinimum\":0,\"exclusiveMaximum\":100}");
	must_pass("excl/in-range", s, instance("50"));
	must_fail_containing("excl/at-min", s, instance("0"),
			     "exclusiveMinimum");
	must_fail_containing("excl/at-max", s, instance("100"),
			     "exclusiveMaximum");
}

static void test_items(void)
{
	fy_generic s;

	s = schema("{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}");
	must_pass("items/all-valid", s, instance("[1,2,3]"));
	must_fail_containing("items/one-invalid", s,
			     instance("[1,\"x\",3]"),
			     "type mismatch");
}

static void test_property_names(void)
{
	fy_generic s;

	s = schema("{\"type\":\"object\","
		   "\"propertyNames\":{\"type\":\"string\"}}");
	must_pass("propNames/valid", s, instance("{\"any-key\":1}"));

	/* All JSON keys are strings, so this always passes; test a pattern. */
	s = schema("{\"type\":\"object\","
		   "\"propertyNames\":{\"type\":\"string\","
		   "\"pattern\":\"^[a-z]+$\"}}");
	must_pass("propNames/pattern-ok", s, instance("{\"abc\":1}"));
	must_fail_containing("propNames/pattern-bad", s,
			     instance("{\"ABC\":1}"), "propertyNames");
}

static void test_nested(void)
{
	fy_generic s;

	s = schema("{\"type\":\"object\","
		   "\"properties\":{\"items\":{\"type\":\"array\","
		   "\"items\":{\"type\":\"object\","
		   "\"properties\":{\"name\":{\"type\":\"string\"}},"
		   "\"required\":[\"name\"]}}}}");
	must_pass("nested/valid", s,
		  instance("{\"items\":[{\"name\":\"a\"},{\"name\":\"b\"}]}"));
	must_fail_containing("nested/missing", s,
			     instance("{\"items\":[{\"name\":\"a\"},{}]}"),
			     "missing required key");
}

static void test_non_standard_type(void)
{
	fy_generic s;

	s = schema("{\"type\":\"custom\"}");
	must_pass("custom/string", s, instance("\"anything\""));
	must_pass("custom/object", s, instance("{\"a\":1}"));
	must_pass("custom/array", s, instance("[1,2]"));
	must_pass("custom/number", s, instance("42"));
}

static void test_format_uri(void)
{
	fy_generic s;

	s = schema("{\"type\":\"string\",\"format\":\"uri\"}");
	must_pass("uri/valid", s, instance("\"https://example.com\""));
	must_fail_containing("uri/invalid", s, instance("\"not a uri\""),
			     "format");
}

static void test_problem_collection(void)
{
	fy_generic s, report, problems;
	size_t count;

	/* Two errors: missing required key + wrong type */
	s = schema("{\"type\":\"object\","
		   "\"properties\":{\"a\":{\"type\":\"string\"}},"
		   "\"required\":[\"a\",\"b\"]}");
	report = validate(s, instance("{\"a\":1}"));
	if (fyai_schema_valid(report)) {
		fprintf(stderr, "FAIL problem_collection: expected invalid\n");
		exit(1);
	}
	problems = fy_get(report, "problems", fy_seq_empty);
	count = fy_len(problems);
	if (count < 2) {
		fprintf(stderr,
			"FAIL problem_collection: expected >=2 problems, got %zu\n",
			count);
		exit(1);
	}
}

int main(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	memset(&test_cfg, 0, sizeof(test_cfg));
	memset(&test_ctx, 0, sizeof(test_ctx));

	test_ctx.cfg = &test_cfg;
	test_ctx.transient_gb = fy_generic_builder_create(&gb_cfg);
	if (!test_ctx.transient_gb)
		return 1;
	test_ctx.durable_gb = test_ctx.transient_gb;

	test_type();
	test_required();
	test_additional_properties_false();
	test_additional_properties_schema();
	test_enum();
	test_const();
	test_any_of();
	test_all_of();
	test_one_of();
	test_not();
	test_string_bounds();
	test_pattern();
	test_numeric_bounds();
	test_items();
	test_property_names();
	test_nested();
	test_non_standard_type();
	test_format_uri();
	test_problem_collection();

	fy_generic_builder_destroy(test_ctx.transient_gb);
	return 0;
}
