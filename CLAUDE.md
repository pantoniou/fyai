# CLAUDE.md

Guidance for working in this repository. See [AGENTS.md](AGENTS.md) for the full
contributor guide and `doc/srd/fyai-srd.md` for the authoritative spec.

## What this is

`fyai` is a **stateless, daemon-less Unix-style AI coding assistant** in C. Each
invocation runs one complete tool-use loop, commits canonical state to a
content-addressed libfyaml arena, and exits. There is no resident process, TUI,
or sidecar state format.

## Architecture invariants (do not break)

- No daemon, resident process, TUI, or hidden process state. One invocation =
  one full loop + state commit + exit.
- Persistent state lives in **libfyaml content-addressed arenas** (under
  `~/.fyai`), never in sidecar formats. Canonical data is immutable,
  deterministic, and address-stable across processes; arena relocation is
  forbidden.
- Keep the tool surface Unix-shaped: read file, write file/patch, run shell
  commands under approval.

## Data model

Use **libfyaml generics** as the native data model — build with
`fy_gb_mapping()`, `fy_gb_sequence()`, etc., not hand-written JSON strings.
Emit compact JSON only at provider boundaries (`FYOPEF_MODE_JSON`) and parse
provider responses straight back into generics. Map provider wire details
(tool-call IDs, request IDs, finish reasons, timestamps) into provider-stream
generics, then derive provider-agnostic content for canonical identity.

Use typed accessor defaults: `fy_get(obj, "content", "")`,
`fy_get(obj, "total_tokens", 0LL)`, `fy_get(obj, "items", fy_invalid)`.

**String lifetime — prefer `fy_castp` over `fy_cast`.** A short string is stored
*inline in the `fy_generic` word*, so the `const char *` from `fy_cast(v, "")`
points into `v` itself. If `v` is a by-value local (e.g. a struct field returned
by value), that pointer dangles once the holder goes out of scope — and only for
short strings, since long ones live in the arena (a nasty, data-dependent bug).
Use `fy_castp(&v, "")`, which takes the *address* of the stored generic and
returns a pointer into that stable storage; compute it at the use site from a
long-lived holder, never cache a `fy_cast` result past the generic's scope.

**Empty strings must survive a YAML round-trip.** An empty-string generic is a
string, not null — but under the core/1.1 schemas a *bare* plain empty scalar
(`key:`) reads back as null. libfyaml's generic emitter therefore quotes an
empty string (`key: ""`) whenever the style is unspecified, so it round-trips
as the empty string in block, pretty and flow output alike. fyai relies on this:
config keys whose default is `""` (e.g. `display/tool_separator`, `prompt_top`)
are stored as empty strings and must re-parse as strings, or the `type: string`
schema check fails on the next commit (an in-session `/config edit` save). Keep
these keys `type: string` in `data/config.schema.yaml` — do not widen them to
accept null to paper over an emitter that drops the quotes.

## Source map

- `src/main.c` — CLI grammar: parse global options, then dispatch a verb or run a
  prompt (`fyai [global-options] <verb|prompt>`).
- `src/commands.c` — verb dispatch (`init`/`dump`/`stats`/`config`/`gc`), the
  `fyai_run` engine runner, and the colorized `fyai_usage` (fy-tool style).
- `src/fyai.c` — core engine orchestration; functional modules live beside it.
- `src/fyai_session.c` — session commands: shared backends for the interactive
  slash commands (`/clear`, `/compact`, `/model`, `/api`, `/context`, plus a
  data-driven table of settings like `/effort`, `/theme`, `/markdown`) and
  the `clear`/`compact`/`context` verbs; the tokenizer-free context estimator
  (bytes/4) and linenoise tab completion (command names, enum values,
  catalogue model names). In the REPL a `/`-prefixed line never reaches the
  model; `//` escapes a literal slash line. `/model` re-runs
  `fyai_config_resolve_model()` and `fyai_request_state_apply()` mid-session,
  re-deriving `<PROVIDER>_API_KEY` unless the key was explicit. Request-shaping
  switches (`/model`, `/api`, the reasoning options, `/temperature`) persist
  into the arena config via the one commit path, so a continuation resumes on
  them (`--transient` keeps the publish in-memory); display settings stay
  session-only. `--new` behaves exactly like `/clear`: it publishes a turnless
  head reset. `/compact`
  makes one tools-off summary call and restarts the chain from the summary,
  keeping the old head under turn metadata `compacted_from`.
- `src/fyai_signal.c` — interactive-session signal handling (installed only in
  the REPL): SIGINT sets a flag the request path polls (the spinner/xferinfo
  callback aborts the in-flight curl transfer, and the tool loop stops issuing
  calls); SIGWINCH calls `linenoiseWindowChanged()` to reflow the prompt.
  Handlers use `sigaction` without `SA_RESTART` so blocking reads/polls see
  EINTR promptly. An interrupted turn keeps the steps that completed: the run
  loop wraps the partial turn (or fy_invalid) with a diagnostic via a manual
  `FYGIF_DIAG` indirect (`fyai_with_diag`/`fyai_report_diag` in fyai.c) — since
  `fy_generic_is_invalid()` dereferences indirects, a diag-wrapped invalid
  still tests invalid everywhere, so no assert-valid churn. Ctrl-Z suspend at
  the prompt and the SIGWINCH reflow live in the vendored linenoise
  (`third_party/linenoise`, fyai-local extensions).
- `src/fyai_render.c` — the one generic-to-Markdown table renderer
  (`fyai_generic_to_markdown()`). Every Markdown table goes through it: a
  mapping renders as a two-column key/value table, a sequence of mappings as
  one column per key. Callers pass a `renderopts` generic (title, preamble,
  `keys` column selection, per-key `columns` overrides of name/align/format)
  rather than a hand-written table; without an override a column's name is the
  humanized key and its alignment is right for numbers/booleans, left
  otherwise. Cells are escaped and truncated, so no value can break a table.
  Build renderopts in the **transient** builder (or frame-locally, in the same
  frame as the call) — never the durable arena; a builder-less `fy_mapping()`
  is `alloca` storage and must not be returned from a helper.
- `src/utils.c` — HTTP response buffers, shell exec capture, generic emit/parse.
  The shell `fork`/`exec` optionally applies a `fyai_sandbox_spec` in the child
  before exec.
- `src/fyai_sandbox.c` — Landlock confinement for shell-tool sub-executions
  (`--sandbox` / config `sandbox`, default off). ABI-probed and masked; grants
  read-only system paths + read/write project (children of the root minus the
  hidden `.fyai`) + temp dirs, denies the rest; applied one-way in the child so
  it is inherited across the exec and every process it spawns. Linux-only: on
  other platforms (macOS) it compiles to no-op stubs behind the same interface,
  where a Seatbelt (`sandbox_init`) back-end would slot in. It is the §4.5/§10
  enforcement floor only; command admission (allow-list/prompt) stays elsewhere.
- `src/fyai_config.c` — layered config loading (arena-resident repo config),
  slash-path config verb (import/export/edit/show/get/set/delete) and the
  global --set/--get/--delete ops.
- `src/fyai_catalog.c` — provider/model catalogue: arena document or embedded
  snapshot, lookups, `catalog` verb.
- `src/*.h` — context structs and internal module interfaces.
- `doc/srd/` — authoritative requirements/design; update when changing
  architecture or invariants.

## Build & run

```sh
cmake -S . -B build
cmake --build build
./build/fyai -m gpt-4o-mini "hello"   # no-verb prompt
./build/fyai dump state               # a verb; see ./build/fyai -h
```

ASAN build (run for parser, storage, tool, or YAML changes):

```sh
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
```

Markdown rendering uses `libfymd4c` directly. The renderer is a required CMake
dependency and provides progressive/healing terminal rendering plus syntax-aware
fenced code blocks through libfyts.

Static build modes:

- `-DFYAI_MOSTLY_STATIC=ON` links fyai's dependencies statically but leaves
  glibc/libm dynamic. Use this for portable glibc binaries; fully-static glibc
  can fail in `getaddrinfo()`/NSS on a different host. Only this mode compiles
  the `src/glibc_compat.c` wrappers for glibc's C23 `__isoc23_*` redirects, so
  GNU C2x builds do not require `GLIBC_2.38` just for `strtol`/`sscanf` family
  calls; fully dynamic builds use the host glibc directly.
- `-DFYAI_MUSL_STATIC=ON` is the fully-static musl mode, intended for the Docker
  builder (`MODE=musl scripts/build-static-docker.sh ./fyai`), which builds
  musl, zlib, OpenSSL, libfyaml, curl, libfymd4c, and libfyts for the same musl
  toolchain.
- `-DFYAI_STATIC=ON` is the legacy fully-static host-libc mode; keep it only for
  controlled same-host/static-glibc experiments.

All static-dependency modes require an installed `libfyaml.a`; glibc modes also
use system `libssl.a`/`libcrypto.a`/`libz.a`. Mostly-static verification should
show only libc/libm/ld.so in `ldd build-static/fyai`; musl/static verification
should show `not a dynamic executable`.

Run the test suite with `ctest --test-dir build --output-on-failure`. Unit
tests (`tests/fyai_patch_test.c`, `tests/fyai_provider_test.c`) run everywhere;
the functional cases (`tests/cases/*.sh`) drive the real binary against the
scenario-driven mock provider (`tests/mock/mock_provider.py`,
`tests/scenarios/*.json`) in a hermetic sandbox — localhost only, private
`HOME`/`XDG_*`. The whole suite, functional cases included, runs under ASAN
builds too.

Three provider modes: Responses API (default), Chat Completions, and the
Anthropic Messages API, selected via `config set api <mode>` / `--set
api=<mode>` (`responses|chat-completions|messages`) — there are no dedicated
`--responses`/`--chat-completions`/`--messages` flags; the config document is
the single place API grammar is chosen, durably, from the command line or
otherwise. Messages mode authenticates with `x-api-key` + `anthropic-version` headers and
always sends `max_tokens`; inbound `tool_use` blocks are normalized to
Responses-style `function_call` items at the parse boundary, so canonical
state, display and cross-provider replay share one shape. Credentials come
from `OPENAI_API_KEY`/`ANTHROPIC_API_KEY` env mappings and the `*.env` files
in the root (all gitignored).

## Configuration

The durable arena root ref is a versioned container mapping
`{fyai: 1, config, catalog, head}` (`fyai_root_decode()` /
`fyai_publish_root()` in `src/fyai_storage.c`; `branches` is reserved). The
repository config is the root's `config` entry — there is no
`.fyai/config.yaml`. `fyai init` ingests an initial config;
`fyai config import|export|edit|show|get|set|delete` operate on the arena
document ($VISUAL/$EDITOR round-trip through a `.yaml` tempfile).

There is a single configuration source: one merged document
(`cfg->config_doc`), built as arena config → `--config` file → `--set`
deltas, deep-merged mapping-wise (`config_merge`). The user file
(`$XDG_CONFIG_HOME/fyai/config.yaml`) is bootstrap-only — used as the base
only when the arena carries no config, never overlaid onto one. The struct
fields are a derived cache filled by a single `apply_config` pass;
`config effective` emits the merged document verbatim, `config show` the
arena entry. Catalog-derived values (endpoint, provider, `max_tokens` from
`max_output_tokens`) are re-derived read-only from the catalogue at resolve
time and never persisted into the config — the config stores intent (the
`model` key), the `api` verb shows the resolved derivation. Compiled-in
defaults still back any key the document does not set. See
`config.yaml.sample`.

A separate, explicitly-informational `catalog:` block *is* persisted: it
mirrors the full catalogue `models[]` entry for the current `model` plus
`canonical_provider` (the model's default, unprefixed provider), read-only
and re-derived on every commit (`catalog_sync_config_doc` in
`src/fyai_config.c`, hooked into `config_doc_sanitize` so every commit path —
`set`/`delete`/`import`/`edit`/`--set`/`--delete` — picks it up, plus the
CLI-overlay path in `apply_config_set_ops` for `config effective` on a
run-local `-m`). It disappears entirely when `model` names something the
catalogue does not know, and `catalog import` re-syncs it against the newly
ingested catalogue for whatever model is already configured.

Config keys are addressed by slash paths of arbitrary depth (`display/color`);
`config get`/`--get` prints one-line flow, `config set`/`--set` parses the
value as a YAML flow document (typed scalars, mappings, sequences), and
`--delete`/`config delete` removes a key. The global `--set`/`--get`/`--delete`
options are repeatable and run once storage opens (`fyai_apply_config_ops`);
`--set` also folds into the current run before model resolution. `--transient`
(`--ephemeral`) stacks an in-memory builder over the durable arena so every
config and state write that session is ephemeral — `ctx->gb` is that builder
(durable_gb otherwise) and the refs-publish is skipped.

Stylistic options live only under the nested `display:` group (markdown,
markdown_mode, color, theme, markdown_theme, code_theme, stream, pretty,
cache_info, stats, tool_preview_lines); the `model` and other options are
top-level. Theming is fully delegated to libfymd4c: `display/markdown_theme`
names one of its embedded themes (`default`, `catppuccin`, `kanagawa`,
`solarized`, `tokyonight`, every one *except* `default` also having a
`-borderless` variant that drops the border glyphs; unset => the library
default), validated via `markdown_theme_valid()` and enumerated for the error
message by `markdown_theme_names()` — both query libfymd4c, so neither can
drift from the embedded set; fyai ships no styling YAML of its own.
`code_theme` overrides the fenced-code (libfyts) highlighter.
The user-turn "bubble" reverse card is read back from the active theme through
`fymd_renderer_get_reverse_pair()`. `theme` means background dark/light only.

The provider/model catalogue (scrape-providers document) is the root's
`catalog` entry (`fyai catalog show|list|import|export`, `src/fyai_catalog.c`),
with a vendored snapshot (`data/catalog.yaml`) embedded at build time as
fallback. Unlike `config`, the catalogue verb is view/import/export only —
there is no in-place edit. A single top-level `model` key drives selection: the catalogue maps
it to the canonical provider's endpoint/grammar/wire-id, validates reasoning
capability, and defaults `max_tokens` from `max_output_tokens`. A model may
carry an optional `provider/` prefix (`openrouter/glm-5.2`) that pins a
specific provider offering it; the prefix is stripped only when it names a
catalogue provider (so a `provider_model_id` containing a slash is left
intact). The resolved provider name is stored on `cfg->provider` for display
and turn metadata. There are no `providers:` presets and no `--provider`/`-P`.

The API key comes from `--api-key`/`-k`, else a config `api_key` env mapping,
else the provider's conventional env var `<PROVIDER>_API_KEY` (name
upper-cased, e.g. `OPENAI_API_KEY`, `OPENROUTER_API_KEY`). `api_key` is never
a raw value — it must be `{ type: env, value: <ENVVAR> }`; every arena
ingestion point (init, import, set, edit) hard-rejects raw keys since the
content-addressed arena cannot forget them.

Project instruction files (`AGENTS.md`, then `CLAUDE.md`) are discovered by
`fyai_project_instructions()` (`src/fyai_config.c`) from a global layer
(`$XDG_CONFIG_HOME/fyai`) plus the project root down to the cwd — walking up
only within a project (nearest `.git`/`.fyai` marker), never the whole tree.
They are concatenated outermost-first (cwd wins recency), each fenced with a
`# <path>` header, and folded onto the base `system_prompt` in `fyai_setup`
just before it is frozen into the canonical system turn. A continuation keeps
the copy it was started with, so editing the files only affects new
conversations.

Reasoning effort is set via the config `reasoning: { effort, summary }`
mapping (`effort`: `minimal|low|medium|high`, `summary`:
`auto|concise|detailed`), e.g. `config set reasoning/effort high` / `--set
reasoning/effort=high`; it maps to the Responses `reasoning` object and Chat
Completions `reasoning_effort`, validated in `fyai_setup`.

Most run-shaping switches (temperature, reasoning, streaming, markdown
rendering/theme, stats, logprobs, token extents, obfuscation/whitewash) have
no dedicated CLI flag at all — they are config keys only, set via `config
set <key> <val>` / `--set <key>=<val>` (see `config.yaml.sample` for the
full key set). A handful of flags remain because they are not config-backed
run-local state: `-C`/`-e`/`-k` name external files/secrets, `-m`/`-u`
resolve through the catalogue, `-t`/`--sandbox` gate tool execution,
`--color`/`--theme`/`--code-theme` are display-only conveniences kept for
ergonomics, `--new`/`-i`/`-d`/`--answer` control process behavior, not
config state, and `--set`/`--get`/`--delete`/`--transient` are the config
mechanism itself.

## Coding style

Linux kernel C style: hard tabs, 8-column stops, kernel braces/spacing, no
whitespace-alignment churn. Declare all local variables in the declaration
block at the start of the function; do not introduce declarations inside
branches, loops, or later statements. Keep declaration initializers out when
the value is assigned by executable code. GNU C2x with `-Wall -Wextra` and
`-Wdeclaration-after-statement`. Four spaces only in CMake. Use
`lower_snake_case` C names and uppercase CMake options (`ENABLE_ASAN`). New
source files get SPDX headers. Favor explicit error handling and clear
ownership; document non-obvious arena/mmap/atomic/durability/filesystem
assumptions locally.

## Commits

Imperative subject with a subsystem prefix, e.g. `cli: add interactive prompt
mode` or `state: garbage collect durable arena`. PRs should note affected SRD
sections and include security notes for approval-policy, sandboxing, network
egress, or tool-execution changes.
