# Declarative state and model access

Status: proposal. Part 1 describes the current state. Part 2 is a design
and a phased plan. Phases 1 to 5 are built: the policy keyword, the
reads, the changes with the review panel and the proposals, revert, keep
and the approval events, and the reads of the catalogue. The items that a
section marks *Remains* are not.

## Goal

Most behaviour of fyai is data. YAML documents state the configuration,
the provider catalogue, the commands, the model tools, the page of the
terminal UI and the skills. C code interprets these documents. Each
document has a JSON Schema, and fyai checks each change against it.

This has two consequences:

1. A person needs one description of this state: what exists, where it is
   stored, how its layers combine, and how a change is checked, recorded and
   undone. Part 1 gives it.
2. The model has no structured access to this state. It must never get
   write access: it presents a change, and fyai applies it. Even a change
   that fyai applies for it is not always safe: a model that can write `sandbox`, `api_url`, `mcp/servers` or
   `catalog_update/command` can leave its confinement or send credentials
   out. Some settings are safe for the model to change. Some need a person
   to approve them. Each change must be reversible. Part 2 gives a tool
   that obeys these rules.

# Part 1 - The declarative state

## Principle

A document states intent. The C code derives the run from it, and does not
keep a second copy of the intent in a structure field (`cfg->config_doc` is
the source; `apply_config` populates the fields in one pass).

Each document has a schema. Each path that ingests a document validates it
with `fyai_schema_validate()` (`src/fyai_schema.c`). The validator supports
a subset of JSON Schema draft 2020-12 (`doc/schema-validator-plan.md`). It
accepts an unknown keyword as an annotation. Thus the `x-fyai-*` keywords
need no support in the validator: the code that reads a document reads them.

## Inventory

| Document | Schema | Stored in | Changed by | Model access today |
|---|---|---|---|---|
| Configuration | `data/config.schema.yaml` | branch `store/config`; session layer; `--config` file; `--set` | `config set/delete/import/edit/reset/undo`, setting commands (`/theme`, `/temperature`, ...), `/session lockdown`, `/session yolo` | none through a tool; see [the shell path](#the-shell-path) |
| Provider catalogue | `data/catalog.schema.yaml` (output of scrape-providers) | branch `store/catalog`; `data/catalog.yaml` is the seed | `catalog set/delete/import/edit/reset/update` and model `settings change`, all through `fyai_catalog_commit()` | provider connection fields are excluded from model changes |
| Commands | `data/command.schema.yaml` | embedded at build time | a build | none |
| Model tools | wire shape, `doc/tools-yaml.md` | embedded at build time | a build | the model gets the specifications of its tools |
| Page document | `data/page.schema.yaml`, then `fyai_page_check()` | embedded `data/page.yaml`, or the file that `display/page` names | configuration | none |
| Page state | `data/page-state.schema.yaml` | built for each frame | fyai | none |
| Skills | `data/skill.schema.yaml` (front matter) | branch `store/skills` | `skill import/new/edit/remove/enable/disable` | the `skill` tool runs a skill |
| Themes | libfymd4c and libfypalette themes | the libraries | `display/theme`, `display/theme_ground` | none |
| Key bindings | libfytimui defaults, `display/keys` | the library; configuration | configuration | none |

The configuration, the catalogue and the skills are branch state. The
other documents are part of the program, or are files that the
configuration names.

## The `x-fyai-*` vocabulary

The keywords bind a schema node to behaviour of fyai.

Command definitions (`data/commands.yaml`, `doc/command-schema-plan.md`):

| Keyword | Effect |
|---|---|
| `x-fyai-positional` | The argument is a positional word, at this index. |
| `x-fyai-rest` | The argument takes the remaining words. |
| `x-fyai-short`, `x-fyai-long` | The option letter and the option name. |
| `x-fyai-negate` | The option has a `--no-` form. |
| `x-fyai-repeat` | The option can occur more than one time. |
| `x-fyai-meta` | The placeholder in help, such as `FILE`. |
| `x-fyai-complete` | The completion kind, from `complete_kinds[]`. |
| `x-fyai-surfaces` | The argument exists on `cli`, `session`, or both. |
| `x-fyai-hidden` | Help and completion do not show the argument. |

Configuration (`data/config.schema.yaml`):

| Keyword | Effect |
|---|---|
| `x-fyai-scope` | For session setting commands, `session` writes the session layer and `stored` commits to the branch. The nearest keyword on the path decides (`fyai_config_session_scoped()`). The `settings` tool stores normal changes regardless of this keyword. |
| `x-fyai-complete` | The completion kind of the value. |
| `x-fyai-model` | What the model can do with the key: `hidden`, `read`, `review` or `write`. The nearest keyword on the path decides (`fyai_config_model_access()`); see [the policy keyword](#the-policy-keyword). |

## Configuration layers

One merged document is built in this order (`config_merge()`,
`fyai_config_adopt_arena()`):

1. the configuration of the active branch (`store/config`);
2. the explicit `--config` file;
3. the `--set` changes of the command line;
4. the session layer (`cfg->config_session`).

The session layer holds the changes that a session made to keys of
`x-fyai-scope: session`. It lives for the invocation only, and `/reload`
carries it to the new process. It is never stored. The `sandbox` and
`display` subtrees are session-scoped, with `display/theme`,
`display/tool_detail` and `display/transcript_system` set back to `stored`.

The user configuration file only initializes an arena that has no stored
configuration. The values that model resolution derives - the endpoint,
the provider, the catalogue limits and the `model_info` block - are not
persisted as intent; `model_info` is informational and is made again on
each commit.

`--transient` puts an in-memory builder above the arena and stores nothing.
`--root` selects one published root and refuses every write.

## Validation points

- `fyai_config_validate_report()` checks the schema, then the semantic
  rules that the schema cannot state, such as the rejection of a raw
  `api_key`. `config_report_commit()` stops a commit that has a problem.
  `config set`, `config import`, `config undo`, `config reset`, `--set`,
  `--config` and the session layer (`config_session_layer_set()`) all use
  it.
- `fyai_catalog_commit()` checks the catalogue against
  `data/catalog.schema.yaml` and derives `model_info` again.
- `skill import`, `skill new` and `skill edit` check the front matter
  against `data/skill.schema.yaml`.
- `fyai_page_load()` checks a page file against `data/page.schema.yaml`,
  and `fyai_page_check()` transcribes each case of it. A file that fails is
  not used: the embedded document draws the page.
- The command registry checks `data/commands.yaml` when it loads.

## History and rollback

Each commit publishes a new root (`fyai_publish_root()`). The branch entry
is one ref-log record, with the operation that made it
(`fyai_branch_op_set()`), and its `store` holds the configuration, the
catalogue and the skills. Thus each earlier configuration is still in the
ref log.

- `config undo N` (`fyai_config_undo()`) walks the ref log back, counts the
  entries whose configuration differs from the next one, and stores the
  configuration of N changes ago.
- `config reset` stores an empty configuration and drops the session layer.
- After a lost CAS, `fyai_branch_store_merge()` merges the store three
  ways, key by key.

A limit: undo replaces the whole document. To undo one change also
removes each later change, also changes that the user made.

## Introspection for a person

| Command | Shows |
|---|---|
| `config show` | the stored configuration |
| `config effective` | the merged document of this invocation |
| `config get PATH` | one value |
| `config describe [PATH]` | the schema node: type, values, default, description |
| `config schema` | the whole schema |
| `config validate` | the problems of the stored document |
| `catalog show/list/tools/schema` | the catalogue |
| `help`, `doc/commands.md` | the commands |
| `/page` | the page document in use and the last frame |

No command says which layer gave an effective value, or how a value is
different from its default.

## The shell path

The model has no tool for this state. Its programs run in a shell, and the
shell is the one place where a program of the model could reach the arena,
for example with `fyai config set sandbox_profile none`:

- With the sandbox on, `fyai_shell_sandbox_begin()` (`src/fyai_tools.c`)
  denies `<project>/.fyai`. An arena outside the project is out of reach
  when no `allow` grant of the profile covers it.
- With the sandbox off (`sandbox_profile: none`), a shell
  program can write the arena.

Part 2 rests on the invariant that the model never modifies the arena. The
sandbox is what holds it against the shell; part 2 records the arena deny
as a precondition of the design.

# Part 2 - Model access

## The invariant

The model never modifies the arena. It presents a change as data in a tool
call: a set of paths and values, and an explanation. fyai validates the change,
decides by the policy whether it needs a person, and applies it through the
commit paths of part 1. The model gets no handle, path or command that
writes state, and no class of the policy gives it one: `write` means that
the application applies the presented change without a question, not that
the model writes.

The shell must not be a way around this. When the shell can reach the
arena (the sandbox is off, or a grant covers the arena), fyai treats each
presented change as `review`, whatever its class, and says why in the
result.

## Requirements

1. The model can read the settings that a task needs, with their schema
   descriptions.
2. The model can present a change. fyai applies it without a question
   only to settings that cannot weaken confinement, conceal the actions of
   the model, run a program, or send data out.
3. A person approves each other change that the model may present.
4. A person can revert each change of the model, one change at a time,
   without losing later changes.
5. When no person can answer, a change waits as a proposal.
6. The policy is data in the schema, beside the setting it governs.
   A setting with no policy is not accessible.

## Threat model

The model can be wrong, or it can work against the user. It controls the
arguments of its tool calls. It cannot type in the terminal, and it cannot
answer a question that fyai puts to the user.

| Class | Effect of a change by the model | Keys |
|---|---|---|
| Escalation | removes confinement | `sandbox`, `sandbox_profile`, `sandbox_profiles`, `sandbox_lockdown`, `agent/isolation`, `agent/transport_isolation`, `agent/spawn`, `view/isolate_session`, `view/scratch_dir` |
| Execution | runs a program the model names | `shell/shell`, `shell/login`, `catalog_update/command`, `mcp/servers`, `mcp/endpoint`, `display/page` (a file the model wrote) |
| Exfiltration | sends credentials or the conversation to another place | `api_url`, `api_key`, `auth`, `no_auth`, `mcp/auth_token`, the OAuth client secrets of `mcp/servers`, `catalog_update/credentials`, `whitewash_api_keys`, `logging/*` |
| Ceiling | removes a limit on the model | `shell/max_timeout_ms`, `shell/hard_max_output_tokens`, `read/hard_max_bytes`, `agent/max_timeout_ms`, `agent/hang_timeout_ms`, `agent/timeout_kill`, `agent/max_live_agents`, `agent/max_branch_depth`, `max_tool_iterations` |
| Persistence | puts text of the model into later requests | `system_prompt`, `parallel_tool_calls_prompt`, `agent/personas`, skills |
| Concealment | hides the actions of the model from the user | `display/tool_detail`, `display/tool_preview_lines`, `display/notice_output`, `display/thinking`, `view/ignore`, `view/gitignore` |
| Spoofing | draws chrome or escapes the model chose | `display/prompt`, `display/prompt_busy`, `display/prompt_top`, `display/prompt_bottom`, `display/session_margin`, `display/keys` (can unbind the key that leaves a tile) |
| Cost | spends money or provider capacity | `model`, `max_tokens`, `retry/*`, `web_search` |

Approval must come from the user:

- not from a parent model: a sub-agent has no settings tool, so no review
  passes through a model on its way to the user;
- not from `--answer`, which answers `ask_user` before the question exists;
- not from a tool, a skill, or an MCP server.

## The policy keyword

`x-fyai-model` on a schema node states what the model can do with the
node and with each node under it:

| Value | Read | Write |
|---|---|---|
| `hidden` | no: the value is left out and its path is listed as hidden | no |
| `read` | yes | no |
| `review` | yes | fyai applies the presented change after the user approves it; else it waits as a proposal |
| `write` | yes | fyai applies the presented change at once; recorded and reversible |

Resolution follows `fyai_config_session_scoped()`: the nearest keyword on
the path decides, and a key that the schema does not name below a node (a
member of a `oneOf` branch, an item of `additionalProperties`) takes the
class of that node. The root has no keyword, so its class is `hidden`.

A change set takes the strictest class of all the paths it touches. A
change that replaces a mapping touches each path under it, also the paths
that only the old or only the new value has. Thus a change of `mcp` cannot
pass a change of `mcp/servers` as a change of a `write` key.

A key that the user sets in `--config` or `--set` keeps its class. The
policy is about who changes a value, not about where the value came from.

## Classification of the configuration

This table is the proposal for the annotations of
`data/config.schema.yaml`. A row with a subtree applies to every key
under it, except the rows below it that name a deeper key.

| Path | Class | Reason |
|---|---|---|
| `view/durability`, `view/track_project`, `view/tool_diff`, `view/undo_depth` | write | storage cost only |
| `view/ignore`, `view/gitignore` | review | concealment: an ignored path leaves the diff and the apply |
| `view/scratch_dir`, `view/isolate_session` | read | escalation |
| `auth`, `api_url`, `no_auth` | read | exfiltration |
| `api_key` | hidden | credential reference; no task needs it |
| `model`, `api` | review | cost; also selects the endpoint |
| `system_prompt`, `parallel_tool_calls_prompt` | review | persistence |
| `parallel_tool_calls` | write | |
| `web_search` | review | egress through the provider |
| `temperature`, `reasoning` | write | |
| `max_tool_iterations` | read | ceiling |
| `max_tokens` | review | cost |
| `branch/on_conflict` | review | can lose turns |
| `tools`, `builtin_shell` | review | changes the tools of later requests |
| `retry` | review | cost; `max_delay_ms: 0` removes the bound |
| `catalog_update` | read | execution |
| `catalog_update/credentials` | hidden | credential names |
| `shell` | write | defaults below the ceilings: `timeout_ms`, `max_output_tokens`, `session_timeout_ms`, `tty*`, `input_poll_ms` |
| `shell/max_timeout_ms`, `shell/hard_max_output_tokens` | read | ceiling |
| `shell/shell`, `shell/login` | read | execution |
| `read/max_bytes` | write | |
| `read/hard_max_bytes` | read | ceiling |
| `agent/timeout_ms` | write | |
| `agent/max_timeout_ms`, `agent/hang_timeout_ms`, `agent/timeout_kill`, `agent/max_branch_depth`, `agent/max_live_agents` | read | ceiling |
| `agent/isolation`, `agent/spawn`, `agent/transport_isolation` | read | escalation |
| `agent/personas` | review | persistence; a persona names a model and a list of tools |
| `sandbox`, `sandbox_profile`, `sandbox_profiles`, `sandbox_lockdown` | read | escalation |
| `logprobs`, `top_logprobs`, `token_extents`, `response_chain` | write | |
| `no_obfuscation` | review | removes a provider defence |
| `whitewash_api_keys`, `logging` | read | exfiltration |
| `arena_dir` | read | |
| `display` | write | presentation |
| `display/tool_detail`, `display/tool_preview_lines`, `display/notice_output`, `display/thinking` | review | concealment |
| `display/prompt`, `display/prompt_busy`, `display/prompt_top`, `display/prompt_bottom`, `display/session_margin` | review | spoofing; can hold escapes |
| `display/keys` | review | spoofing; can leave the user in a tile |
| `display/page` | read | execution: names a file on disk |
| `model_info` | read | derived |
| `mcp/enabled` | review | connects to servers |
| `mcp/startup_wait`, `mcp/timeout` | write | |
| `mcp/endpoint`, `mcp/protocol_version`, `mcp/servers` | read | execution and exfiltration |
| `mcp/auth_token`, `mcp/servers/*/auth/client/secret` | hidden | credential references |

A session setting command writes a session-scoped key to the session layer:
such a change ends with the invocation. A normal `settings change` tool call
stores the key in the branch. Only `trial: true` uses the session layer.

## A master setting

One new key, `model_access`, of class `read`, sets a ceiling for the whole
policy:

| Value | Effect |
|---|---|
| `none` | The tool is not offered. |
| `read` | Reads only. |
| `review` | Each change needs approval, also of a `write` key. The default. |
| `write` | The classes of the schema apply. |

The setting can only make the schema stricter. No value lets the model
change a `read` key.

## The tool `settings`

One tool, defined in `data/tools.yaml`. Its entry is in
`src/fyai_settings.c`, with `FYAI_TOOL_PARENT` (it reads and changes the
state of the parent) and `FYAI_TOOL_EFFECT_NONE`. Its `describe` function
leaves the tool out when `model_access` is `none`; a later phase also
removes the actions that `model_access` does not allow from the
specification, so the model learns what this run permits.

Parameters: `action`, `path`, `document` (`config`, the default, or
`catalog`), and
the arguments of the action. A path is a slash path; a segment `*` stands
for any name of a mapping of names, such as an MCP server or a persona. A
name that holds a '/' or a '~' writes them as `~1` and `~0`, the escapes
of JSON Pointer (RFC 6901): the key binding `Ctrl-/` is
`display/keys/prompt/Ctrl-~1`. libfyaml reads the same escapes
(`fy_gb_path_split()`), so `config get` and `--set` agree with the tool,
and every path that fyai writes escapes its names. A segment of digits
that meets a mapping names its integer key, else the string key of those
digits. A '~' followed by anything but `0` or `1` is not a path.

The schema is large, so no answer gives the whole of it. The model finds a
key with `search`, and reads the one key it needs with `describe`.

| Action | Arguments | Result |
|---|---|---|
| `search` | `query`, `path`, `limit` | The keys under `path` whose path or description holds each word of `query`: those that hold every word in their path first. Each key has its access and the first sentence of its description. At most `limit` keys (20, up to 50), and the count found. |
| `describe` | `path` | The schema node without its members and without the `x-fyai-*` keywords, the access, the scope, the value in force, the layer that gave it (`default`, `branch`, `invocation`, `session`), and the members with their access and first sentence. |
| `get` | `path` | The value in force and its layer. Hidden members are left out and listed in `hidden`. |
| `list` | `path`, `changed` | The leaf keys under the path with access, layer and value, at most 100; `changed: true` gives only values that differ from the default. |
| `select` | `path` or `from`, `where`, `fields`, `sort`, `order`, `limit` | The rows that meet every condition of `where` (`eq`, `ne`, `lt`, `le`, `gt`, `ge`, `has`, `contains`, `exists`), sorted by one field - a row without it last - and cut to `limit` (20, up to 100), each with the `fields` asked for. A row of the configuration is a key under `path`, with `key`, `access`, `scope`, `layer`, `value`, `default` and `changed` (the value differs from the default); hidden keys are no rows. A row of the catalogue is an item at `from`, where a segment `*` takes every item and adds a column that names it: `providers/*/models` gives each offering with `provider`. `pricing/blended` is made as three input tokens to one output token. The answer gives the count matched. |
| `export` | `document`, `format`, `shape` | Write the document to a file for a program, such as jq, to search: the configuration without its hidden keys, as a `tree` or as the `rows` that `select` reads, or the catalogue; `json` or `yaml`. fyai names the file, in a directory of its own made by `mkdtemp()` under `$TMPDIR` (`ctx->settings_export_dir`); the model names no path. Cleanup removes the directory; a forked child forgets it without removing it. |
| `change` | `changes: [{path, value} or {path, delete: true}]`, `explanation`, `trial` | The presented change set and a Markdown explanation of it. A normal change is stored in the branch; a trial lasts for the invocation. fyai answers `applied`, `pending` or `refused`, with an id, the diff and the cause. |
| `revert` | `id` | Take back one change of the model of `write` keys, as a change of its own that the same rules apply or review. A key changed since is a conflict, and nothing is taken back. |
| `proposals` | none | The proposals of this branch: id, status, paths and changes. |

The title row of a call names what it reads or changes: the query and
the path of a search, the key of a read, the first key of a change (with
the count of the others, and `trial`), or the id of a revert. Every text
in it that the model wrote is a code span that the text cannot close. The
result for the user (`settings_format()`) is short: a row for each key of
a search or a list, one row for a read, and for a change or a revert the
outcome - applied as `cN`, waiting as `pN`, or rejected with the reply of
the user - and the whole diff that fyai made. The entry has
`FYAI_TOOL_WHOLE_RESULT`, so the preview of the tool body does not cut that
diff. The model gets the result as JSON text.

Each result has `arena_protected: true|false`: true when a view holds the
arena as a descriptor, or when the sandbox is on and the arena is inside
`<project>/.fyai`, which the sandbox denies. An arena elsewhere is not known
to be denied, so it is false. When it is false, every change is handled as
`review` (see [the invariant](#the-invariant)).

### A change

The model presents the change; each step below is work of fyai.

1. Build the merged document with the change set applied, as
   `config_session_layer_set()` and `fyai_config_set()` do.
2. Validate it with `fyai_config_validate_report()`. A problem refuses the
   change and names the path.
3. Find the class of the change set, raised to `review` when
   `arena_protected` is false or `model_access` is `review`. `hidden` and
   `read` refuse it, and
   the result names each path and its class. The refusal is a result for
   the model, not a diagnostic for the user.
4. `write`: publish with the operation `config/model`, record the change
   (below), call `fyai_config_rederive()`, and present the diff on the
   title row of the call. No change is silent.
5. `review`, with a person present: show the review panel (below).
6. `review`, without a person - a one-shot prompt, an ACP or SDK host, or
   no UI: store a proposal and return `pending`.

The new values take effect at the next model request of the turn. A
change of `model`, `tools` or `builtin_shell` therefore changes the tools
and the endpoint of that request.

`trial: true` writes the change to the session layer only, for any key.
Nothing is stored but the entry of the change log. The
change ends with the invocation, unless the user stores it with
`config keep ID`. A trial is the natural way to try a setting and drop it.

### The review panel

A change that needs review is a question to the user, shown as `ask_user`
shows one: a panel in the input area, made with `fyai_ask_normalize()` and
`fyai_ui_ask()`, with the same keys. There is no second question UI.

The panel has three answers:

| Answer | Effect | Result for the model |
|---|---|---|
| *Reject* | Nothing changes. Escape and `^C` also reject. | `rejected` |
| *Accept* | fyai applies the change. | `applied`, with the id |
| Typed text | Nothing changes. | `rejected`, with the text as the user's reply, so the model can present a revised change |

*Reject* is the first option and stands under the cursor. Enter alone
rejects, and so does an Enter that arrives in one chunk with a pasted
reply, which the panel reads before it takes the typed text. To accept
takes Down and Enter, or the key `2`.

The panel has two parts, with two authors:

- **The proposal**, written by the model: the `explanation` of the call. It
  is Markdown, rendered as an answer of the model is rendered, with prose,
  lists, tables and `mermaid` diagrams. It says why the change is wanted
  and what it is expected to do.
- **The diff**, written by fyai alone, from the change set. The model
  supplies none of its text.

The proposal can argue; the diff states the facts. The panel must not let
the first pass for the second:

- **Two regions.** The proposal and the diff are separate slots of the
  panel, each under a fixed heading that fyai writes: one names the
  proposal as the text of the model, the other names the diff as made by
  fyai. The proposal is rendered at the width of its slot and cannot draw
  outside it, so it cannot draw a heading, a rule or a row of the diff
  slot.
- **The proposal has no raw terminal control.** Every control byte of the
  explanation but a line break and a tab becomes `?` before the render, so
  the proposal cannot set a colour, move the cursor or write an OSC
  sequence. A fenced `diff` block in the proposal is drawn as code of the
  proposal, in its slot, never as the diff.
- **The diff is made by fyai.** fyai emits the changed paths from the
  configuration before and after the change, and shows their unified diff
  in the `diff` slot. A trial compares the session layer. YAML file headers
  let the renderer highlight the configuration syntax.
- **The diff cannot be forged by a value.** The diff panel reads unified
  diff data directly. A value in that data cannot open page markup or a
  Markdown block. Control bytes become `?` before the review.
- **The diff covers the changed paths.** It shows each value under a
  replaced subtree that the model may read. A
  summary row counts the keys and the lines added and removed and names
  the layer, so a diff cut at the bottom of its slot cannot hide its size.
- **The heading is fixed.** The headings of both slots and the question are
  written by fyai; the question names the number of keys and the access.
  The model cannot write them.
- **The diff keeps its rows.** The options and proposal stand on the left,
  and the diff stands on the right. The diff view holds all rendered rows
  and the default page gives it the height of the left column.
  Page Up and Page
  Down move through it with one row of overlap. Its scroll bar shows the
  position; its arrows move one row and its track moves one page.
- **What was shown is what is applied.** Accept applies the document that
  the panel diffed. A pending proposal is checked and diffed again when the
  user reviews it.

*Remains:* a note when a higher layer (`--config`, `--set`) holds the same
key, so that the change has no effect in this invocation; a scroll of each
slot alone; and a new diff when another process commits the configuration
while the panel waits, which the publish now merges three ways instead.

The same panel reviews every document of part 2: the diff of a skill
covers its `SKILL.md` and each supporting file, and the diff of a page
document or of `display/keys` covers that document. `config approve` in a
session shows the same panel for a pending proposal, diffed against the
configuration in force when the user reviews it, not when the model
presented it.

### Sub-agents

The settings tool belongs to the session of the user. Its entry has
`FYAI_TOOL_ROOT_ONLY`, so a delegated agent is not offered it, and a call
that a delegated agent makes anyway is refused. The configuration of a run
is the user's to change; a sub-agent that needs another setting says so to
the agent that delegated it, whose model can present the change.

A sub-agent never communicates with the user directly: it reaches the user
only through its parent. Without the tool there is no review to forward,
and no proposal stored on the branch of a sub-agent.

### The change log

Two new members of the branch store:

```text
store/config_changes:   [{name: cN, status, access, paths, changes, trial, persistent,
                          before, after, diff, explanation, origin, time}]
store/config_proposals: [{name: pN, ...the same members}]
```

- `changes` is the change set as the model presented it; `before` and
  `after` map each path to its value in the layer it writes, or null;
  `diff` is the whole unified diff that fyai made.
- `persistent` marks changes that store all paths in the branch. Older
  entries without it retain their schema-scoped revert behavior.
- `origin` is the branch of the agent that made the change.
- `status` of a change is `applied`, or `session` for one that wrote only
  the session layer, then `reverted` or `kept`; of a proposal `pending`,
  `approved` or `rejected`. An entry that reverts or keeps another names it
  in `of: {id, status}`.
- Both are sequences of named items, so `fyai_branch_store_merge()` merges
  them by name after a lost CAS, and `fyai_branch_store_build()` keeps them
  as members it does not know.
- Each keeps its newest 64 entries.
- An ID is symbolic and per branch (`c1`, `c2`, ...). It is not an arena
  address.
- A change is published with the operation `settings`, the configuration
  and the entry in one ref-log entry. An approval publishes the change and
  the new status of the proposal in one entry too.

### Revert

`config revert ID` restores the `before` value of each path of the
change, three ways: a path whose current value is not the `after` value of
the change is a conflict. The revert then changes nothing and names each
such path. With `--force` it takes back the other paths.

The revert is a change of its own, in the change log, and the entry it
takes back becomes `reverted` in the same publish. A key that had no value
before is removed; a parent that the removal leaves empty is removed too
when the schema makes it a plain mapping with no default, so a change that
made `reasoning: {effort: high}` leaves no `reasoning: {}` behind. A parent
of a union, such as `sandbox`, stays.

This is a revert of one change. Unlike `config undo`, it keeps each later
change. `config undo` stays as it is.

The model can revert only its own changes of `write` keys, through the
same decision as a change: applied unasked when the setting and the shell
allow it, else reviewed or proposed. The user can revert any change.

`config keep ID` stores a trial or an older session-only change: each key
goes to the branch configuration with the value of that change, and leaves
the session layer. The entry becomes `kept`.

### User commands

New subcommands of `config` in `data/commands.yaml`, with surfaces `cli`
and `session`. No tool calls them.

| Command | Effect |
|---|---|
| `config changes` | The change log, newest first: id, status, access, keys, branch, time. |
| `config proposals` | The proposals, newest first, with the same columns. |
| `config review ID` | The diff that fyai makes of a pending proposal now, then the explanation of the model as a block quote, so a heading in it stays inside the quote. |
| `config approve ID` | Apply a pending proposal. A session shows the review panel first; a verb applies it, because the user named it, and shows the diff it applied. A proposal that is no longer valid is refused. |
| `config reject ID` | Mark a pending proposal rejected. |
| `config revert ID [--force]` | Take back a change, as the section above says. |
| `config keep ID` | Store a trial change. |

`config approve` is an asynchronous command. In a session it puts the
proposal in the review panel and returns; the panel answers through its
callback, which applies the proposal and ends the command. It never waits
in a loop of its own inside the session. Reject and Escape leave the
proposal pending.

When a session approves or rejects a proposal, fyai queues the event
`[config 'ID' approved]` or `[config 'ID' rejected]`, with the result as
JSON, for the model, as `src/fyai_asks.c` queues the answer of a
background question. A verb tells the model nothing: no model runs.

`config describe` also shows the class of the node and the layer of the
effective value. `config effective --origin` lists the layer of each value,
and `--changed` lists only values that differ from their default. These
two views answer the questions that part 1 found open.

## Other documents

The class of a document whose schema fyai does not own is fixed in the
table of documents of the tool, not in that schema.

| Document | Class | Notes |
|---|---|---|
| Catalogue | read and review/write | `document: catalog` selects it. `search` matches model names, capabilities, modalities and providers. `get`, `list`, `select` and `export` read it. `change` accepts sourced edits under `models/NAME` or `providers/NAME/models/ID` only and checks the whole document against `data/catalog.schema.yaml`. It uses the same `model_access`, arena protection, review panel and `config` proposal commands as a configuration change. Provider connection fields cannot be changed by this action. |
| Skills | review | `change` makes a proposal of a new or an edited skill. The front matter is checked against `data/skill.schema.yaml`. The review shows the whole `SKILL.md`, each file, `context: fork` and each tool that `allowed-tools` adds. A model never sets the executable bit; the review can grant it. |
| Theme | write | `display/theme`, from `markdown_theme_selectors()`. |
| Key bindings | review | `display/keys`. The reserved keys stay refused. |
| Page document | review | A proposal stores the page document in the branch store, and `display/page` names the stored document. A model never names a file on disk. |
| Commands, model tools | read | Embedded. `describe` returns the definition of a command, so the model can tell the user how to do a task. |

## Phases

Each phase is three patches: implementation, tests, documentation.

1. **The policy.** Resolve `x-fyai-model` with the walk that
   `fyai_config_session_scoped()` uses, as one shared function. Annotate
   `data/config.schema.yaml` as the table above states. Add
   `model_access`. Unit tests: each leaf of the schema resolves to a class;
   no key of the escalation, execution, exfiltration or ceiling classes is
   `write` or `review`; no key of the concealment or spoofing classes is
   `write`; a change set takes the strictest class of its subtree.
2. **Read.** The tool with `search`, `describe`, `get` and `list`, and
   redaction. `config describe` shows the access in its `model` column.
   Built. `config effective --origin` and `--changed` remain.
3. **Change.** `change` with the `write`, `review` and proposal paths,
   the change log, and `config changes/proposals/approve/reject`.
   Built, with `config review`. Cases: `tests/cases/settings_review.sh`
   drives the panel in a terminal, `tests/cases/settings_proposals.sh` the
   proposals.
4. **Revert and trial.** `revert`, `config revert/keep`, `trial` and the
   approval events: built, with `tests/cases/settings_revert.sh` and
   `tests/cases/settings_approve.sh`. A sub-agent has no settings tool.
5. **Catalogue read and edits.** Built, with `tests/cases/settings_catalog.sh`.
6. **Skill proposals.**
7. **Display documents:** theme, keys, and stored page documents.

## Open questions

1. Is a limit of model changes per turn needed, or does the review class
   suffice?
2. Should `read` show the endpoints of `mcp/servers`? They name hosts, not
   credentials, but they tell the model where data goes.
3. When the arena is not protected, is `review` for every change enough,
   or should the tool offer reads only?
4. Should `config undo` skip the entries of the change log, so that a user
   undo and a model change do not interleave?
