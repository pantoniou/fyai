# Finishing MCP support: OAuth for remote servers

Status: **not started**. The substrate landed with `src/fyai_oauth.c`
(commits `76cb534`, `e313200`); nothing in `src/fyai_mcp.c` uses it yet.

## Where MCP auth stands today

A remote (`streamable-http`) server carries exactly one credential: a static
bearer token, resolved by `mcp_server_auth_token()` in `src/fyai_mcp.c` from a
`{ type: env, value: <ENVVAR> }` secret indirection (per-server `auth_token`,
falling back to the group `mcp.auth_token`). It is sent as
`Authorization: Bearer <token>` on every request. There is no discovery, no
refresh, no interactive login, and a 401 is reported rather than acted on.

That is fine for servers issuing long-lived PATs (the GitHub MCP server, see
`doc/GITHUB_MCP.md`), and insufficient for anything implementing the MCP
authorization spec, where the server expects an OAuth 2.1 access token obtained
by the client.

## What is already reusable

`src/fyai_oauth.c` is provider-agnostic and was split out with this exact
second caller in mind:

- `fyai_oauth_pkce_generate()` — verifier / S256 challenge / CSRF state.
- `fyai_oauth_flow_start()` — arms the loopback redirect receiver on a
  **borrowed** loop and returns; advances as a state machine. This matters
  here: an MCP login is triggered mid-turn, so it must not stall the turn.
  Do **not** reach for `fyai_oauth_flow_wait()` from the MCP path — that is the
  sync convenience for `auth login`, which has nothing else to do.
- `fyai_oauth_flow_finish()` / `_destroy()`, `fyai_oauth_open_browser()`,
  `fyai_oauth_query_value()`.

`fyai_auth.c` is the worked example, but note what it does *not* share: the
issuer, client id, scopes, authorize query and token-exchange schema are all
compiled in there on purpose (a comment there explains why — letting repo
config redirect subscription credentials would be a real hole). MCP is the
opposite case: everything is discovered.

## What is missing

1. **Authorization server discovery.** On a 401, read the
   `WWW-Authenticate` header's `resource_metadata`, fetch
   `/.well-known/oauth-protected-resource`, then the AS's
   `/.well-known/oauth-authorization-server` (RFC 8414) for the authorize and
   token endpoints. None of this exists; `fyai_oauth.c` deliberately has no
   discovery in it, and this should probably live in a new
   `mcp_oauth_discover()` rather than being pushed down into the shared module.

2. **Dynamic client registration (RFC 7591).** MCP clients generally have no
   pre-registered client id. POST to the AS's `registration_endpoint`, keep the
   issued `client_id` (and secret, if any). Needs somewhere durable to live —
   see storage below.

3. **Token storage and refresh.** Access + refresh tokens, per (server,
   issuer), with expiry. `fyai_auth.c` stores its single credential in
   `$XDG_STATE_HOME/fyai/auth.json` plus an optional keyring; MCP needs several,
   keyed by server. **Not the arena** — it is content-addressed and cannot
   forget a secret, which is the same reason `api_key` is env-indirection only.
   Extending the `auth.json` shape to a keyed map is the least-surprising route;
   `auth_lock()`/`auth_save()` already handle the locking.

4. **The 401 retry path.** `fyai_auth_should_retry()` / `_prepare_retry()` are
   the model-request analogue; MCP needs the same shape around
   `mcp_request()` — one refresh attempt, then one interactive login, then fail.

5. **Consent.** An MCP server should not be able to make fyai open a browser
   unprompted. Gate the interactive login behind the same approval surface as
   tool execution, and make it refusable in a non-interactive run (`ask_abort`
   is the existing precedent).

6. **Config surface.** `auth_token` stays for PAT servers. Add something like
   `auth: { type: oauth }` per server, defaulting to the current bearer
   behaviour so no existing config changes meaning.

## Testing

`tests/fyai_oauth_test.c` covers the receiver. What MCP OAuth adds needs its
own coverage, and the mock provider is the right host: `tests/mock/` can grow a
stub authorization server so discovery, registration, exchange and the 401
retry are all exercised hermetically, with no network. Follow the existing
`tests/scenarios/*.json` pattern.

## Sequencing note

The single application loop has landed, so an MCP login armed mid-turn is just
another set of sources on `fyai_ctx_loop(ctx)` — no nested run required. Use
`fyai_oauth_flow_start()` with a completion callback and let the turn's own run
drive it; reaching for `fyai_oauth_flow_wait()` would drive the loop to
completion and defeat the point.
