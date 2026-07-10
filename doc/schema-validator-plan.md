# JSON Schema Validator — Implementation Plan

## Goal

A rudimentary JSON Schema (draft 2020-12 subset) validator using libfyaml
generics, good enough to parse and validate tool schemas from the provider/model
catalogue (`data/catalog.yaml`). The validator follows the report convention of
`fyai_config_validate_report`.

## Report convention

Returns a **report mapping** (not a bool):

```
{ result: "ok" }                                  # success
{ result: "failed", problems: [ "msg", ... ] }     # failure
```

Callers check `fy_equal(fy_get(report, "result"), "ok")` and iterate
`fy_get(report, "problems")` for diagnostics — same shape as the config
validator.

## API (`src/fyai_schema.h`)

```c
fy_generic fyai_schema_validate(struct fy_generic_builder *gb,
                                fy_generic schema,
                                fy_generic instance);

static inline bool fyai_schema_valid(fy_generic report);
void fyai_schema_report_print(fy_generic report);
```

Problems carry a path prefix (e.g. `"questions/0/options/1/label: expected
string"`). `schema_problem_add()` mirrors `config_problem_add()`.

## Draft 2020-12 support contract

The validator implements the following draft 2020-12 subset. Boolean schemas
are supported wherever a schema may appear: `true` accepts every instance and
`false` rejects every instance.

| Category   | Keywords |
|------------|----------|
| Type       | `type` (string or array); standard types + non-standard (`custom`, `web_search`, `image_generation`, etc.) = accept anything |
| Object     | `properties`, `patternProperties`, `required`, `dependentRequired`, `dependentSchemas`, `additionalProperties` (bool or schema), `propertyNames`, `minProperties`, `maxProperties` |
| Array      | `prefixItems`, `items` (single-schema form), `minItems`, `maxItems`, `uniqueItems`, `contains`, `minContains`, `maxContains` |
| String     | `minLength`, `maxLength`, `pattern` (POSIX ERE), `format` (`uri` best-effort, others skip) |
| Number     | `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`, `multipleOf` |
| Equality   | `const`, `enum` (JSON numeric and structural equality) |
| Combining  | `anyOf`, `allOf`, `oneOf`, `not`, `if`/`then`/`else` |
| References | Local fragment `$ref` using JSON Pointer, including `$defs` targets |

- `allOf`: all subschemas must pass (collect all problems).
- `oneOf`: exactly one subschema must pass.
- `not`: subschema must fail.

### Accepted annotations and extensions

`$schema`, `$comment`, `title`, `description`, `default`, `examples`,
`deprecated`, `readOnly`, `writeOnly`, `contentEncoding`, `contentMediaType`,
and `contentSchema` do not affect validation and are accepted as annotations.
Unknown keywords are accepted as extensions; provider catalogues use custom
keywords on opaque tool schemas, so treating every unknown key as an error
would reject valid catalogue data. Unknown `format` names are likewise treated
as annotations. The only asserted format is the validator's best-effort `uri`
check.

### Recognized but unsupported

Encountering one of these standardized keywords makes validation fail with an
`unsupported JSON Schema keyword` problem instead of silently accepting a
schema whose semantics fyai cannot honor:

- identifiers and advanced references: `$id`, `$anchor`, `$dynamicRef`,
  `$dynamicAnchor`, `$vocabulary`;
- evaluated-location applicators: `unevaluatedProperties`,
  `unevaluatedItems`;
- legacy spellings not implemented by this draft subset: `definitions`,
  `dependencies`, `additionalItems`.

An external URI `$ref` fails with `external $ref is unsupported`; a plain-name
fragment such as `#name` fails with `anchor $ref is unsupported`. Only `#` and
`#/...` JSON Pointer references into the current root schema are supported.

Regexes use POSIX ERE rather than the ECMA-262 dialect recommended by JSON
Schema. This is a documented compatibility limitation because patterns cannot
be classified reliably by dialect before compiling them.

## Implementation (`src/fyai_schema.c`)

Core recursive function:

```c
static fy_generic schema_validate(struct fy_generic_builder *gb,
                                  fy_generic root, fy_generic schema,
                                  fy_generic instance, const char *path,
                                  fy_generic problems, unsigned int depth);
```

- `path` built with `asprintf` as we descend; freed per level.
- Validation runs in a deduplicating child builder with its own allocator,
  stacked on the caller's builder. `problems` accumulates there as a sequence;
  only the final report is internalized into the caller before the child is
  destroyed. Throwaway branch diagnostics therefore do not survive a call.

Type dispatch:

| JSON Schema | libfyaml check |
|-------------|----------------|
| `object`    | `fy_generic_is_mapping` |
| `array`     | `fy_generic_is_sequence` |
| `string`    | `fy_generic_is_string` |
| `integer`   | `fy_generic_is_int` |
| `number`    | `fy_generic_is_int \|\| fy_generic_is_float` |
| `boolean`   | `fy_generic_is_bool` |
| `null`      | `fy_generic_is_null_type` |
| other       | skip (accept) |

For `pattern`: `regcomp`/`regexec` (REG_EXTENDED | REG_NOSUB), `regfree` after.
For `format: uri`: best-effort scheme-prefix check; others skip.

Combining keywords run recursive validation with throwaway problem sequences
when only branch pass/fail state is required.

## Wiring

Lands as standalone module + tests. No changes to tool dispatch yet; a
follow-up can call it before `fyai_tool_run_one` and return a
`tool error: schema: <problem>` result on failure.

## Tests (`tests/fyai_schema_test.c`)

Same pattern as `fyai_provider_test.c`. Cases:

1. type — pass/fail, array-of-types
2. required — missing key fails
3. additionalProperties: false — extra key fails
4. additionalProperties as schema
5. enum — in/out
6. const — equal/unequal
7. anyOf — one branch / none
8. allOf — all pass / one fails
9. oneOf — exactly one / zero / two
10. not — matching instance fails
11. minLength/maxLength
12. pattern — match/miss
13. minimum/maximum/exclusiveMinimum/exclusiveMaximum
14. items — array element validation
15. propertyNames — key validation
16. Nested objects — recursion
17. Non-standard types — accept anything
18. Catalog fixture — validate against embedded catalogue tool schema
19. Problem collection — two errors both reported

## CMake

- Add `src/fyai_schema.c` to `fyai` target.
- Add `fyai_schema_test` executable (sources: `tests/fyai_schema_test.c`,
  `src/fyai_schema.c`, `src/fyai_sandbox.c`, `src/utils.c`; same link libs).
- `add_test(NAME fyai/schema_validate COMMAND fyai_schema_test)`.

## Out of scope

- External `$ref` resolution and dynamic references.
- `unevaluatedProperties` and `unevaluatedItems`; these require evaluated
  location tracking across references and combining/applicator keywords.
- Schema dialect/vocabulary negotiation.
- Structured error objects (plain strings match config report style).
- Full `format` assertion (only `uri`, best-effort).
- ECMA-262 regular expressions (the implementation uses POSIX ERE).
