# Repository Guidelines

## Project Structure & Module Organization

`fyai` is a C project for a stateless Unix-style AI coding assistant. Treat [doc/srd/fyai-srd-v0_11.md](/mnt/980-linux/panto/work/fyai/doc/srd/fyai-srd-v0_11.md) as authoritative.

- `src/` contains the first-party C sources and headers; `src/main.c` is the
  CLI entry point.
- `CMakeLists.txt` defines the build, GNU C2x flags, ASAN option, install rules, and libfyaml discovery.
- `doc/srd/` contains requirements and design rationale. Update it when changing architecture or invariants.
- `cmake/` holds build helpers such as tags/cscope support.
- `scripts/` holds build-time utilities such as `git-version-gen`.
- `build/` is generated local output; do not edit or commit it.

Future modules should follow SRD boundaries: CLI, configuration, approvals, providers, tools, arena/session storage, schema, bundles, and GC.

## Architecture Constraints

Preserve the core design: no daemon, resident process, TUI, or hidden process state. Each invocation runs a complete tool-use loop, commits canonical state, and exits. Persistent state belongs in libfyaml content-addressed arenas, not sidecar formats. Canonical data must remain immutable, deterministic, and address-stable across processes; persistent arena relocation is forbidden.

Keep the tool surface Unix-shaped: read file, write file/patch, and execute shell commands under approval.

## libfyaml Generic Usage

Use libfyaml generics as the native data model for provider requests, responses, configuration, session state, tool calls, and future arena records. Build values with `fy_gb_mapping()`, `fy_gb_sequence()`, and related builder APIs instead of hand-written JSON strings. Emit compact JSON only at provider boundaries and parse provider JSON responses immediately back into generics.

The current `src/main.c` prototype is the reference style: construct request mappings, emit with `FYOPEF_MODE_JSON`, validate by parsing back, and inspect with the same emitter. Keep `--pretty` dumps for debugging, but do not let display formatting become canonical state.

Accessor defaults matter. Use typed defaults such as `fy_get(obj, "content", "")`, `fy_get(obj, "total_tokens", 0LL)`, and `fy_get(obj, "items", fy_invalid)`. For emitted string generics, `fy_cast(emitted, "")` returns a pointer valid for the builder lifetime; do not duplicate it unless ownership must outlive the builder.

Map provider-specific wire details into provider-stream generics, then derive provider-agnostic content generics for canonical identity. Tool-call IDs, request IDs, finish reasons, and timestamps belong in provider streams or metadata, not semantic content.

## Configuration

Configuration is YAML, parsed with libfyaml (`src/fyai_config.c`). Three
inputs are consulted: the user config (`$XDG_CONFIG_HOME/fyai/config.yaml`,
else `~/.config/fyai/config.yaml`), the repository config (the arena root's
`config` entry — there is no `.fyai/config.yaml`), and an explicit file
passed with `--config`/`-C` (which must exist when named). See
`config.yaml.sample` for the full key set.

There is a single configuration source: one merged document, built as arena
(repository) config → `--config` file → `--set` deltas, deep-merged
mapping-wise. The user file is bootstrap-only — it is the base only when the
arena carries no config, and is never overlaid onto one. `config effective`
emits the merged document verbatim; built-in defaults back any key it does
not set. Catalogue-derived values (endpoint, provider, `max_tokens`) are
re-derived read-only at resolve time and never persisted into the config —
the config stores intent, the `api` verb shows the resolved derivation.

**Model & provider.** A single top-level `model` key drives selection; the
catalogue (`fyai catalog list`) maps it to the canonical provider's endpoint
URL, API grammar and wire model-id. A `provider/` prefix (`openrouter/glm-5.2`)
pins a specific provider offering the model — the prefix is honoured only when
it names a catalogue provider. There are no `providers:` presets and no
`--provider`/`-P`; the API key comes from `--api-key`/`-k`, a config `api_key`
env mapping, or the provider's `<PROVIDER>_API_KEY` env var.

**Config editing.** Keys are slash paths of arbitrary depth. `fyai config
get|set|delete <key>` and the repeatable global `--get`/`--set`/`--delete`
options share one implementation: `get` prints one-line flow, `set` parses the
value as a YAML flow document (typed). `--transient`/`--ephemeral` keeps the
whole session (config edits and conversation state) in memory only.

**Project instructions.** `AGENTS.md` and `CLAUDE.md` files are ingested into
the system prompt of a *new* conversation: discovered from a global layer
(`$XDG_CONFIG_HOME/fyai`) and from the project root (nearest `.git`/`.fyai`)
down to the cwd, concatenated outermost-first so the most specific directory
wins recency, and folded onto the base `system_prompt` before it is frozen as
the canonical system turn (`fyai_project_instructions()`,
`src/fyai_config.c`). Continuations keep the frozen copy; editing the files
affects only new conversations.

**Reasoning effort** is controlled by `--reasoning-effort`
(`minimal|low|medium|high`) and `--reasoning-summary` (`auto|concise|detailed`),
or a config `reasoning: { effort, summary }` mapping. It maps to the Responses
API `reasoning` object and to Chat Completions `reasoning_effort` (summary is
Responses-only). Values are validated in `fyai_setup`; omit to leave the model
default untouched.

*Canonical-schema note (deferred).* Per SRD §6.1–6.2 and decision-log §2.2,
reasoning effort/summary are **sampling parameters**: they belong in the
metadata-events layer, never in canonical content, so two turns with identical
content at different effort levels stay canonical-equal. The canonical mode is a
provider-agnostic ordinal (`minimal|low|medium|high`, optionally a token
budget); per-provider wire spellings (Responses `reasoning:{effort,summary}`,
Chat `reasoning_effort`, Anthropic `thinking:{budget_tokens}`, OpenRouter
`reasoning:{effort|max_tokens}`) belong at the provider-streams boundary.
Current code carries the ordinal in `fyai_cfg` (correct) but hard-maps only the
two OpenAI shapes inline in `fyai_run_model_step`; move this to a per-provider
translation step, and route effort/summary into metadata-events (and any
exposed reasoning *text* into canonical content as a content element), when the
three-layer turn schema lands.

**Token usage stats** are printed to stderr at end of run with `--stats` (or
config `stats: true`): accumulated `input`, `cached`, `output`, `reasoning`
(when present), and `total` across every model call in the session. Wire field
names differ per API mode (`prompt_tokens`/`input_tokens`,
`completion_tokens`/`output_tokens`, `*_tokens_details.cached_tokens`) and are
normalized in `fyai_extract_usage`. Stats go to stderr to keep stdout the
canonical content stream (SRD §4.6). The normalized usage is also persisted on
each turn (a `usage` mapping, parallel to `provider_stream`), so the `stats`
verb (`fyai_show_stats`) reports the cumulative total summed over the turn chain.

**Secrets are never raw values.** `api_key` must be an indirection mapping
`{ type: env, value: <ENVVAR> }`; a raw string, an unknown type, or an unset
variable is rejected at load time.

**`--env <file>`** sources a `.env` file before secrets are resolved. Lines are
`KEY=VALUE` or `export KEY=VALUE`, value optionally single/double quoted, with
**no variable substitution** (`FOO="bar"` ok; `FOO="${BAR}"` and `FOO=$BAR` are
errors). Only variables fyai actually uses are exported: the direct `OPENAI_*`
names plus every variable named by a `{ type: env, value: NAME }` mapping in the
loaded configs (collected by `collect_env_refs`); any other line is parsed but
ignored.

## Command Grammar

The CLI is `git`/`gh`-shaped (`src/main.c` parses globals then dispatches via
`src/commands.c`):

```
fyai [global-options] <verb> [verb-args]
fyai [global-options] <prompt...>
```

`getopt_long` uses a leading `+` so it stops at the first non-option token.
That token is dispatched as a **verb** iff it matches a known verb
(`fyai_is_verb`); otherwise the remaining arguments are run as a **prompt**.

Verbs: `init [path]` (create `./.fyai`, optionally copy a config),
`dump [state|anchors|providers]`, `stats [--json]` (cumulative usage summed
from per-turn `usage` metadata in the arena),
`config [show|get <k>|set <k> <v>|delete <k>|import|export|edit]`,
`list [providers|models|turns] [--brief|--full]`, `catalog`,
`clear` (publish a null head; old turns stay in the arena until gc),
`compact [hint]` (one summary model call restarts the chain from the summary;
the old head is kept as `compacted_from` turn metadata),
`context` (context-window fill from the catalogue `context_window`, the last
recorded call's usage, and a tokenizer-free bytes/4 estimate), `gc`.
Management verbs need no API key. The verb-less run keeps all run
modifiers as global options. `fyai_usage` (fy-tool-style colorized help) lists
them; `-h` prints it.

The interactive REPL (`-i`, or verb-less with a tty) dispatches `/`-prefixed
lines as slash commands sharing the verb backends (`src/fyai_session.c`):
`/clear`, `/compact [hint]`, `/model [name]` (mid-session catalogue
re-resolution + request-state rebuild), `/context`, `/stats`,
`/help`, `/exit`/`/quit`, and a data-driven family of settings
(`/effort`, `/summary`, `/theme`, `/markdown-theme`, `/code-theme`,
`/markdown`, `/stream`, `/print-stats`, `/temperature`) with tab completion for
command names, enum values and catalogue model names. A slash line never
reaches the model; `//...` escapes to send a literal slash-prefixed prompt.
Request-shaping switches (`/model`, `/api`, the reasoning options,
`/temperature`) persist into the arena config so a continuation resumes on
them; display settings stay session-only, with `config set` / `--set` as the
durable forms.

The REPL installs signal handlers (`src/fyai_signal.c`, interactive only —
batch keeps the default dispositions): Ctrl-C during a model request/tool run
aborts just that turn and returns to the prompt, **persisting the steps that
completed** (the in-flight call's diagnostic rides on the result generic via a
manual `FYGIF_DIAG` indirect, `fyai_with_diag`/`fyai_report_diag`); Ctrl-Z
suspends from the prompt and resumes cleanly; resizing the terminal reflows
the prompt live (SIGWINCH → `linenoiseWindowChanged()`). The mock provider
supports a per-step `delay` so the interrupt/abort path is tested
deterministically (`tests/cases/interrupt*.sh`).

## Build, Test, and Development Commands

```sh
cmake -S . -B build
cmake --build build
./build/fyai -m gpt-4o-mini "hello"     # no-verb prompt
./build/fyai dump state                 # a verb
```

Enable sanitizers:

```sh
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
```

Run the tests with `ctest --test-dir build --output-on-failure`: C unit tests
plus mock-provider functional cases (`tests/cases/*.sh` against
`tests/mock/mock_provider.py`; not registered under ASAN builds because the
libfyaml durable arena's fixed mapping collides with the ASAN allocator).

## Coding Style & Naming Conventions

This project follows the Linux kernel coding style for C source: hard tabs, 8-column tab stops, kernel-style braces and spacing, and no whitespace-only alignment churn. C targets GNU C2x with `-Wall -Wextra`. Use four spaces only for CMake. Prefer lower_snake_case C names and uppercase CMake options such as `ENABLE_ASAN`. New source files should include SPDX headers.

Favor explicit error handling and clear ownership rules. Document non-obvious arena, mmap, atomic, durability, and filesystem assumptions locally.

## Testing Guidelines

Add tests through CTest as modules appear. Use stable names such as `fyai/arena_init` or `fyai/config_layers`. Prioritize SRD invariants: fixed-base arena mapping, local-filesystem refusal, refs CAS ordering, YAML round trips, approval-policy matching, and provider-independent canonical content. Run ASAN builds for parser, storage, tool, and YAML changes.

## Commit & Pull Request Guidelines

Recent commits use short imperative subjects with a subsystem prefix, for example `cli: add interactive prompt mode`. Prefer `area: concise summary`.

Pull requests should include affected SRD sections, summary, build/test commands, and compatibility notes for arena/schema changes. Include security notes for approval policy, sandboxing, network egress, or tool execution changes.
