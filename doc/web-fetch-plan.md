# Native URL fetch: design

Status: draft, no code. Closes gap G3 of
[`tool-gap-analysis.md`](tool-gap-analysis.md).

## Problem

Hosted `web_search` runs at the provider and only on endpoints that declare
it. On any other endpoint the model cannot read a page that the user or a
search result names. The model can run `curl` through `exec_command`, but that
result has no source, no limits, and no replayable citation, and it needs
network access in the sandbox.

## Tool

`web_fetch` is a local tool: a normal entry in `data/tools.yaml` with its
`struct fyai_tool_def`, defined in a new `src/fyai_web.c`. It is not hosted
and does not replace `web_search`; when both are offered, the description of
`web_fetch` says that it reads a known URL.

Arguments:

| Name | Type | Meaning |
| --- | --- | --- |
| `url` | string, required | An absolute `http` or `https` URL. |
| `offset_bytes` | integer | Continue after a truncated result. Same meaning as in `read_file`. |
| `max_bytes` | integer | Cap on the returned bytes. The configured ceiling limits it. |

Result, a mapping built in `ctx->transient_gb`:

```yaml
url: https://example.org/a          # as requested
final_url: https://example.org/b    # after redirects
redirects: [https://example.org/a]  # hops, in order
status: 200
content_type: text/html
title: Example page                 # HTML <title>, else absent
retrieved: 2026-10-16T09:12:44.103211Z
bytes: 48213                        # size of the decoded body
truncated: true
next_offset_bytes: 65536            # only when truncated
content: "..."
```

Failure returns `ok = false` with the cause: the stage (policy, DNS, connect,
TLS, redirect, size, type, timeout), the URL, and the status. A diagnostic is
raised with `FYAI_MODULE`, so the user sees the same reason.

The metadata is part of the stored tool result. Replay, export, and the
foreign-import writer need nothing else for a citation. The title row is a
`tool_head` fragment over the URL and the outcome, drawn by the one tool-head
renderer.

### Content handling

- `text/*`, `application/json`, `application/xml`, `application/yaml`,
  `application/javascript`, and `+json`/`+xml` types are accepted. The type
  list is `web_fetch/types`.
- HTML is reduced to text with a small extractor in `src/fyai_web.c`: drop
  `script`, `style`, `noscript`, and `template`; keep headings, lists, links
  (`[text](url)`), preformatted blocks, and tables as rows. It reads a
  bounded window and keeps no DOM. A page with no readable text returns the
  raw window and says so.
- A PDF, an image, or another binary type fails with a cause that names the
  type. Image input is a separate gap (G6).
- Content encoding: `CURLOPT_ACCEPT_ENCODING` is set, and the cap counts the
  decoded bytes, so a small compressed body cannot expand without limit.
- The charset is converted to UTF-8; invalid bytes are replaced and the
  result sets `lossy: true`.

## Trust boundary

A fetched page is data from an unknown author. Three rules apply.

1. The description of the tool says that the content is data and not an
   instruction. This is guidance, not a barrier.
2. Every result carries its origin (`final_url`, `retrieved`). A reader of the
   transcript, or of an export, can tell which text the model read from the
   web.
3. The tool cannot reach anything that the user did not allow. Policy is
   enforced in C, below.

The tool sends data out: a URL can carry a secret in its path or query. The
model can already do this with `curl` in the `none` sandbox profile, so the
`none` profile gains no new power. In the `lockdown` profile the shell has
no network, so `web_fetch` must not be a way around it (see Policy).

## Policy

The Landlock network rules confine the shell child only. A fetch runs in
fyai, which they do not cover, so the tool needs a policy of its own. The
configuration group `web_fetch` states it.

| Key | Default | `x-fyai-model` | Meaning |
| --- | --- | --- | --- |
| `enabled` | `true` | `read` | Offer the tool. |
| `max_bytes` | 262144 | `read` | Default window. |
| `max_bytes_limit` | 2097152 | `hidden` | Ceiling for a value that the model supplies. |
| `timeout_ms` | 20000 | `read` | Time limit for one fetch, redirects included. |
| `max_redirects` | 5 | `read` | Hops that the tool follows. |
| `types` | list above | `read` | Accepted content types. |
| `allow` | `[]` | `hidden` | Hosts (`example.org`, `*.example.org`) that are permitted even if private. An empty list allows any public host. |
| `deny` | `[]` | `hidden` | Hosts that are refused. `deny` wins. |
| `proxy` | `env` | `hidden` | `env` honors the proxy variables, `none` goes direct. |

Every key that lifts a limit or widens reach is `read` or `hidden`, as the
rules of the schema require. `config/model_access` lists `web_fetch/allow`,
`web_fetch/deny`, and `web_fetch/proxy` as closed.

Enforcement:

- Schemes: `CURLOPT_PROTOCOLS_STR` is `http,https`. `CURLOPT_FOLLOWLOCATION`
  is off. The tool reads each `Location` itself, checks it as a new request
  (scheme, host lists, hop count), and records it in `redirects`. A redirect
  from `https` to `http` fails.
- Addresses: a loopback, private, link-local, unique-local, multicast, or
  metadata address (`169.254.169.254`, `fd00:ec2::254`) is refused unless the
  host matches `allow`. The check runs on the address that curl connects to,
  in `CURLOPT_OPENSOCKETFUNCTION`, not on the name. A name that resolves to a
  public address first and a private address later (DNS rebinding) is thus
  refused at connect time. The same check covers an IP literal and each
  redirect hop.
- Credentials: no cookies, no `Authorization`, no environment, and no
  configured header. The request is `GET` with `Accept` and a fixed
  `User-Agent`. The URL userinfo (`user:pass@`) is refused.
- Lockdown: with `sandbox_profile: lockdown`, the tool is offered only when
  `web_fetch/allow` names hosts, and it then fetches only those. An empty
  `allow` removes the tool from the run through `describe`, so the model does
  not see a tool that always fails.
- `--root` and a read-only invocation: the tool writes nothing and may run.
- Sub-agents inherit the configuration and may call the tool. A skill that
  forks keeps its `allowed-tools` filter.

## Execution

- The tool has `effect: FYAI_TOOL_EFFECT_NONE`, no `in_parent`, and runs in a
  tool job, so independent fetches of one response run in parallel.
- The transfer uses `fyai_curl_submit()` on the invocation's loop. The write
  callback stops the transfer at `max_bytes + 1` and reports `truncated`.
  Cancellation calls `fyai_curl_cancel()` and completes the request.
- The deadline is armed by `fyai_tool_job_submit()` and is not armed again in
  the child. The `timeout_ms` of the config sets the curl limit for the one
  transfer as a backstop.
- A transient failure (`fyai_http_transient()`) is retried once with a
  backoff, for a request that has returned nothing. The model otherwise gets
  the failure and decides.
- The tool keeps no cache and writes nothing to the arena beyond the tool
  result that the conversation stores anyway. There is no state to share
  between processes.
- A child process that forks without `exec` uses the existing
  `fyai_ctx_fork_disown()` rules; the tool adds no live context state.

## Integration

- `data/tools.yaml`: add the entry and update the order comment, and the order
  list in `doc/tools-yaml.md`.
- `src/fyai_claude_sdk.c`: map `WebFetch` to `web_fetch` beside `WebSearch`,
  so the SDK permission rules apply.
- `doc/foreign-tool-mapping.md`: map the `WebFetch` call of a foreign session.
  An imported call stays inert.
- `data/config.schema.yaml`: the `web_fetch` group, with a description for
  each key. `settings` and `fyai catalog tools` show it with no more code.
- `tests/fyai_tool_spec_test.c`: the description is byte-identical to the file.
- `doc/tool-gap-analysis.md`: move G3 from open to implemented.

## Tests

All cases are hermetic: a localhost server in the mock provider harness, a
private `HOME`, and `web_fetch/allow: [127.0.0.1]` because loopback is private.

Unit tests (`tests/fyai_web_test.c`): URL and host matching; the address
classes, v4 and v6, mapped addresses included; the redirect decision; the HTML
extractor on fixtures; the type list; the charset conversion; the window and
`next_offset_bytes`.

Functional cases:

1. fetch a text page and a JSON page; check the result fields.
2. HTML page: script and style removed, title and links kept.
3. truncation, then a second call with `next_offset_bytes`.
4. a gzip body that expands above the cap stops at the cap.
5. redirect chain recorded; a loop, too many hops, and `https`-to-`http` fail.
6. a redirect to a denied host and to a private address fail with the cause.
7. a name that resolves to a private address is refused with no `allow`.
8. binary type and wrong status fail with a readable cause.
9. slow server: the timeout fires; `cancel` and `^C` end the transfer.
10. `lockdown` removes the tool unless `allow` is set.
11. replay and export show the title row and the stored metadata.
12. a sub-agent calls the tool and its diagnostics reach the parent.
13. `--root` run reads and does not write.

The DNS-rebinding case uses a resolver stub through `CURLOPT_RESOLVE`; it needs
no real DNS.

## Patch series

1. `tools: add the web_fetch tool` (config group, schema keys, run function,
   policy, extractor, SDK mapping).
2. `tests: cover the web_fetch tool`.
3. `doc: describe the web_fetch tool` (this file becomes a short reference,
   `tools-yaml.md`, `user-guide.md`, the gap analysis).

## Open decisions

1. **HTML extractor or raw window.** The extractor is the largest piece of
   code. Without it the model reads raw HTML and wastes most of the window on
   markup. Recommendation: write the small extractor, no DOM.
2. **Ask before an unlisted host.** The default allows any public host. The
   alternative asks the user once per host in an interactive session and
   denies in a headless run. It matches the exfiltration concern but adds a
   prompt path to every fetch. Recommendation: allow by default, because the
   shell already has the same reach in `none`, and keep `deny`.
3. **Proxy.** `env` honors `HTTPS_PROXY`, which a corporate network needs, but
   a proxy sees the policy bypass at its own address: the address check then
   runs on the proxy, not on the target. Recommendation: with a proxy, check
   the host name against the host lists and skip the address check, and say so
   in the documentation.

## Downloads (second step)

The first patch returns text to the model and writes nothing. A download is a
later, separate change: the optional argument `save_to`, a project-relative
path.

- The body goes to a temporary file in the target directory and is renamed
  when it is complete. An existing file is not replaced.
- The path passes the checks of `write_file`: the project root, the arena
  deny, and the project view of an isolated agent. The file has no execute
  bit.
- `web_fetch/max_download_bytes` limits the size; the type list does not
  apply.
- The result has no `content`. It holds the path, `bytes`, `sha256`,
  `final_url`, `content_type`, and `retrieved`.
- The policy of the first patch applies without change.
- The `effect` of a call depends on its arguments: `FYAI_TOOL_EFFECT_FILES`
  when `save_to` is set. Check how the scheduler orders such a call before the
  change.
