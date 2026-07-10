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
	must_pass("type/integral-float", s, instance("42.0"));
	must_fail("type/fractional-float", s, instance("1.5"));
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

static void test_boolean_schema(void)
{
	fy_generic s;

	must_pass("boolean-schema/true", schema("true"), instance("42"));
	must_fail("boolean-schema/false", schema("false"), instance("42"));

	s = schema("{\"type\":\"object\",\"properties\":{\"x\":false}}");
	must_fail("boolean-schema/property", s, instance("{\"x\":1}"));

	s = schema("{\"type\":\"array\",\"items\":false}");
	must_pass("boolean-schema/empty-items", s, instance("[]"));
	must_fail("boolean-schema/items", s, instance("[1]"));

	s = schema("{\"type\":\"object\",\"additionalProperties\":false}");
	must_fail("boolean-schema/additional-property", s, instance("{\"x\":1}"));

	s = schema("{\"not\":false}");
	must_pass("boolean-schema/not", s, instance("1"));

	s = schema("{\"type\":\"object\",\"propertyNames\":false}");
	must_pass("boolean-schema/empty-property-names", s, instance("{}"));
	must_fail("boolean-schema/property-names", s, instance("{\"x\":1}"));
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

static void test_pattern_properties(void)
{
	fy_generic s;

	s = schema("{\"type\":\"object\",\"patternProperties\":{"
		   "\"^x-\":{\"type\":\"integer\"}},"
		   "\"additionalProperties\":false}");
	must_pass("patternProperties/pass", s, instance("{\"x-one\":1}"));
	must_fail("patternProperties/type", s, instance("{\"x-one\":\"no\"}"));
	must_fail_containing("patternProperties/additional", s,
			     instance("{\"other\":1}"), "additional property");

	s = schema("{\"type\":\"object\",\"properties\":{"
		   "\"x\":{\"minimum\":0}},\"patternProperties\":{"
		   "\"^x$\":{\"maximum\":10}}}");
	must_pass("patternProperties/both", s, instance("{\"x\":5}"));
	must_fail("patternProperties/both-properties", s, instance("{\"x\":-1}"));
	must_fail("patternProperties/both-pattern", s, instance("{\"x\":11}"));
}

static void test_dependencies(void)
{
	fy_generic s;

	s = schema("{\"type\":\"object\",\"dependentRequired\":{"
		   "\"credit_card\":[\"billing_address\"]}}");
	must_pass("dependentRequired/absent", s, instance("{}"));
	must_pass("dependentRequired/present", s,
		  instance("{\"credit_card\":1,\"billing_address\":\"x\"}"));
	must_fail_containing("dependentRequired/missing", s,
			     instance("{\"credit_card\":1}"), "requires key");

	s = schema("{\"type\":\"object\",\"dependentSchemas\":{"
		   "\"name\":{\"required\":[\"age\"]}}}");
	must_pass("dependentSchemas/absent", s, instance("{}"));
	must_pass("dependentSchemas/present", s,
		  instance("{\"name\":\"n\",\"age\":1}"));
	must_fail("dependentSchemas/fail", s, instance("{\"name\":\"n\"}"));
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

static void test_conditional(void)
{
	fy_generic s;

	s = schema("{\"type\":\"object\",\"if\":{\"properties\":{"
		   "\"kind\":{\"const\":\"number\"}},\"required\":[\"kind\"]},"
		   "\"then\":{\"properties\":{\"value\":{\"type\":\"number\"}},"
		   "\"required\":[\"value\"]},\"else\":{\"properties\":{"
		   "\"value\":{\"type\":\"string\"}},\"required\":[\"value\"]}}");
	must_pass("conditional/then", s,
		  instance("{\"kind\":\"number\",\"value\":1}"));
	must_fail("conditional/then-fail", s,
		  instance("{\"kind\":\"number\",\"value\":\"x\"}"));
	must_pass("conditional/else", s,
		  instance("{\"kind\":\"text\",\"value\":\"x\"}"));
	must_fail("conditional/else-fail", s,
		  instance("{\"kind\":\"text\",\"value\":1}"));
}

static void test_local_ref(void)
{
	fy_generic s;

	s = schema("{\"$defs\":{\"positive\":{\"type\":\"integer\","
		   "\"minimum\":1}},\"$ref\":\"#/$defs/positive\"}");
	must_pass("ref/basic", s, instance("2"));
	must_fail("ref/basic-fail", s, instance("0"));

	s = schema("{\"$defs\":{\"a/b~c\":{\"const\":1}},"
		   "\"$ref\":\"#/$defs/a~1b~0c\"}");
	must_pass("ref/escaped", s, instance("1"));
	must_fail("ref/escaped-fail", s, instance("2"));

	s = schema("{\"$defs\":{\"\":{\"type\":\"string\"},"
		   "\"0\":{\"type\":\"integer\"}},\"anyOf\":["
		   "{\"$ref\":\"#/$defs/\"},{\"$ref\":\"#/$defs/0\"}]}");
	must_pass("ref/empty-key", s, instance("\"x\""));
	must_pass("ref/numeric-map-key", s, instance("1"));

	s = schema("{\"$defs\":[{\"type\":\"boolean\"}],"
		   "\"$ref\":\"#/$defs/0\"}");
	must_pass("ref/sequence-index", s, instance("true"));
	must_fail("ref/sequence-index-fail", s, instance("0"));

	s = schema("{\"$ref\":\"#/$defs/missing\",\"$defs\":{}}");
	must_fail_containing("ref/unresolved", s, instance("1"),
			     "unresolved local $ref");

	s = schema("{\"$ref\":\"#\"}");
	must_fail_containing("ref/cycle", s, instance("1"),
			     "recursion limit");

	s = schema("{\"$ref\":\"https://example.com/schema\"}");
	must_fail_containing("ref/external", s, instance("1"),
			     "external $ref is unsupported");

	s = schema("{\"$ref\":\"#named\"}");
	must_fail_containing("ref/anchor", s, instance("1"),
			     "anchor $ref is unsupported");
}

static void test_unsupported_keywords(void)
{
	fy_generic s;

	s = schema("{\"unevaluatedProperties\":false}");
	must_fail_containing("unsupported/unevaluated", s, instance("{}"),
			     "unsupported JSON Schema keyword 'unevaluatedProperties'");

	s = schema("{\"$id\":\"https://example.com/schema\"}");
	must_fail_containing("unsupported/id", s, instance("1"),
			     "unsupported JSON Schema keyword '$id'");

	s = schema("{\"properties\":{\"absent\":{"
		   "\"unevaluatedItems\":false}}}");
	must_fail_containing("unsupported/unvisited-branch", s, instance("{}"),
			     "<schema>/properties/absent");

	/* Annotations and provider extension keywords remain permissible. */
	s = schema("{\"description\":\"x\",\"x-provider-option\":true}");
	must_pass("unsupported/extensions-accepted", s, instance("1"));
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

	s = schema("{\"type\":\"string\",\"minLength\":1,\"maxLength\":1}");
	must_pass("len/unicode-codepoint", s, instance("\"é\""));
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

static void test_multiple_of(void)
{
	fy_generic s;

	s = schema("{\"type\":\"integer\",\"multipleOf\":3}");
	must_pass("multipleOf/integer", s, instance("12"));
	must_fail_containing("multipleOf/integer-fail", s, instance("10"),
			     "multiple");

	s = schema("{\"type\":\"number\",\"multipleOf\":0.1}");
	must_pass("multipleOf/decimal", s, instance("0.3"));
	must_fail("multipleOf/decimal-fail", s, instance("0.35"));
}

static void test_property_bounds(void)
{
	fy_generic s;

	s = schema("{\"type\":\"object\",\"minProperties\":1,"
		   "\"maxProperties\":2}");
	must_pass("properties/min", s, instance("{\"a\":1}"));
	must_pass("properties/max", s, instance("{\"a\":1,\"b\":2}"));
	must_fail_containing("properties/too-few", s, instance("{}"),
			     "minProperties");
	must_fail_containing("properties/too-many", s,
			     instance("{\"a\":1,\"b\":2,\"c\":3}"),
			     "maxProperties");
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

static void test_prefix_items(void)
{
	fy_generic s;

	s = schema("{\"type\":\"array\",\"prefixItems\":["
		   "{\"type\":\"string\"},{\"type\":\"integer\"}],"
		   "\"items\":{\"type\":\"boolean\"}}");
	must_pass("prefixItems/pass", s, instance("[\"x\",1,true,false]"));
	must_fail("prefixItems/first", s, instance("[1,1]"));
	must_fail("prefixItems/second", s, instance("[\"x\",\"no\"]"));
	must_fail("prefixItems/items", s, instance("[\"x\",1,2]"));

	s = schema("{\"type\":\"array\",\"prefixItems\":[true],"
		   "\"items\":false}");
	must_pass("prefixItems/closed-pass", s, instance("[1]"));
	must_fail("prefixItems/closed-fail", s, instance("[1,2]"));
}

static void test_unique_items(void)
{
	fy_generic s;

	s = schema("{\"type\":\"array\",\"uniqueItems\":true}");
	must_pass("uniqueItems/pass", s, instance("[1,2,{\"a\":1}]"));
	must_fail_containing("uniqueItems/scalar", s, instance("[1,2,1]"),
			     "not unique");
	must_fail_containing("uniqueItems/numeric-equality", s,
			     instance("[1,1.0]"), "not unique");
	must_fail_containing("uniqueItems/structural", s,
			     instance("[{\"a\":1},{\"a\":1}]"),
			     "not unique");
	must_fail_containing("uniqueItems/structural-numeric", s,
			     instance("[{\"a\":1},{\"a\":1.0}]"),
			     "not unique");
}

static void test_contains(void)
{
	fy_generic s;

	s = schema("{\"type\":\"array\",\"contains\":{\"type\":\"integer\"}}");
	must_pass("contains/default-min", s, instance("[\"x\",1]"));
	must_fail_containing("contains/default-min-fail", s,
			     instance("[\"x\",true]"), "contains matched");

	s = schema("{\"type\":\"array\",\"contains\":{\"type\":\"integer\"},"
		   "\"minContains\":2,\"maxContains\":3}");
	must_pass("contains/range", s, instance("[1,2,\"x\"]"));
	must_fail("contains/too-few", s, instance("[1,\"x\"]"));
	must_fail("contains/too-many", s, instance("[1,2,3,4]"));

	s = schema("{\"type\":\"array\",\"contains\":false,\"minContains\":0}");
	must_pass("contains/boolean-min-zero", s, instance("[1,2]"));
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
	must_fail_containing("uri/numeric-scheme", s, instance("\"1:test\""),
			     "format");
	must_fail_containing("uri/punctuation-scheme", s, instance("\"+:test\""),
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
	test_boolean_schema();
	test_required();
	test_additional_properties_false();
	test_additional_properties_schema();
	test_pattern_properties();
	test_dependencies();
	test_enum();
	test_const();
	test_any_of();
	test_all_of();
	test_one_of();
	test_not();
	test_conditional();
	test_local_ref();
	test_unsupported_keywords();
	test_string_bounds();
	test_pattern();
	test_numeric_bounds();
	test_multiple_of();
	test_property_bounds();
	test_items();
	test_prefix_items();
	test_unique_items();
	test_contains();
	test_property_names();
	test_nested();
	test_non_standard_type();
	test_format_uri();
	test_problem_collection();

	fy_generic_builder_destroy(test_ctx.transient_gb);
	return 0;
}
