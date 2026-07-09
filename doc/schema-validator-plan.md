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

## Supported keywords

| Category   | Keywords |
|------------|----------|
| Type       | `type` (string or array); standard types + non-standard (`custom`, `web_search`, `image_generation`, etc.) = accept anything |
| Object     | `properties`, `required`, `additionalProperties` (bool or schema), `propertyNames` |
| Array      | `items` (single-schema form) |
| String     | `minLength`, `maxLength`, `pattern` (POSIX ERE), `format` (`uri` best-effort, others skip) |
| Number     | `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum` |
| Equality   | `const` (`fy_equal`), `enum` (iterate, `fy_equal`) |
| Combining  | `anyOf`, `allOf`, `oneOf`, `not` |

- `allOf`: all subschemas must pass (collect all problems).
- `oneOf`: exactly one subschema must pass.
- `not`: subschema must fail.

**Ignored metadata:** `$schema`, `description`, `default`, `$ref`/`ref`.

## Implementation (`src/fyai_schema.c`)

Core recursive function:

```c
static fy_generic validate(struct fy_generic_builder *gb,
                           fy_generic schema, fy_generic instance,
                           const char *path, fy_generic problems);
```

- `path` built with `asprintf` as we descend; freed per level.
- `problems` accumulates as a sequence.

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

`anyOf`/`oneOf`/`not` use a `validate_one` returning bool (discard
sub-problems); synthesise a single top-level problem on failure.

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

- `$ref` resolution (decorative in catalogue).
- `prefixItems`/tuple validation.
- `dependentRequired`/`dependentSchemas`.
- Structured error objects (plain strings match config report style).
- Full `format` validation (only `uri`, best-effort).
