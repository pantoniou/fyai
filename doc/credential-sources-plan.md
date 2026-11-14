# Plan: one component for every outbound credential

This is the design of `src/fyai_cred.c`. It extends section 5 of
`doc/agent-transport-isolation-sdd.md`.

## 1. Purpose

fyai sends a credential on many paths. Each path resolves, refreshes and
retries it in its own code. The transport already has the right scheme for the
paths that it serves: a credential source with a name, `prepare` before a
request, `reject` after a 401, a tag for a token, and one refresh shared by
all waiting requests. The plan makes that scheme the only one. Every path
below uses one component, with or without transport isolation.

Out of scope: the inbound check of a listener (`fyai serve --listen`, the
bearer verifier of `fyai_remote_http.c`). It decides whether to accept a peer
and holds no credential to send.

## 2. The paths today

| Path | Source | Holder | Refresh | Gap |
| --- | --- | --- | --- | --- |
| Model request, API key | `api_key` setting: env, secret store, `mem:cli`, auto | agent, or transport | none | the value is read at setup; a rotated key needs a restart; no isolation of `auto` chain beyond the transport profile |
| Model request, ChatGPT login | login store (`oauth:chatgpt`) | agent, or transport | on 401, per request, forced | header built once; no tag test; N refreshes for N requests |
| Model discovery (`list models`) | the login | agent only; refused with isolation | its own chain | needs an egress profile (SDD section 7) |
| Catalogue update | child program with `catalog_update/credentials` names | the child | none | a separate rule: names, not values |
| MCP HTTP, static token | `auth_token` env mapping, `mcp_auth_token` | agent | none | env only; no secret store; read at connect |
| MCP HTTP, OAuth | discovery, registration, authorize, token, refresh | agent | timer at expiry minus 300 s, no store lock | a refresh form of its own; no 401 path; no tag; no isolation |
| MCP stdio | environment given to the child | the child | none | sanitize rule only |
| Provider login (`auth login`, MCP authorize) | browser flow (`fyai_oauth.c`) | a verb or the agent | n/a | acquire, not lifecycle |
| `--remote` client | launch token, pinned TLS key | the client | none | not a model credential; listed for completeness |

The setting `api_key: {type, value}` already becomes the string `type:name`
(`api_key_ref`), and the transport profile names its source as `env:NAME`,
`secret:NAME`, `mem:NAME`, `oauth:chatgpt`, chained with `|`. The two syntaxes
are one syntax.

## 3. The component

A credential source is a table entry, and the transport registry
(`credential_sources[]`) is the registry of the process:

```c
struct fyai_cred_ops {
	const char *prefix;      /* "env:", "secret:", "mem:", "oauth:" ... */
	int  (*get)(src, char **secret, struct curl_slist **extra);
	int  (*prepare)(src, waiter, bool resumed);  /* NULL: never due */
	int  (*reject)(src, waiter, uint64_t tag, bool probe);
	void (*schedule)(src);                       /* NULL: no timer */
};
```

- A static source (`env:`, `secret:`, `mem:`) has `get` only. A `reject`
  probe says no, so a 401 is the response.
- A refreshable source (`oauth:chatgpt`, `oauth:mcp:NAME`) adds `prepare`,
  `reject` and `schedule`. All of them share one engine:
  one refresh for each source, a list of waiters, the store lock, the tag test.
- The waiter is a callback and a userdata. It runs once, with the result of the
  refresh. The users are: a parked transport request, a model request of the
  agent, a model discovery request, an MCP HTTP request, and the blocking
  wrapper of a standalone command.
- The chain `A|B` and the headers (`FYAI_TA_BEARER`, `FYAI_TA_HEADER`, the
  `extra` list) stay as `fyai_transport_headers_build()` makes them. The agent
  path calls the same builder, so a request has one way to get its headers.
- A request is authenticated through `fyai_cred_prepare()`: it returns 0 when
  the credential is ready, 1 when the request waits for a refresh, or a
  negative errno. `ctx->headers` does not hold a copy of a login token: the
  headers of a request are built when the request is sent.

### 3.1 Acquire is not lifecycle

Login, registration and authorize establish a credential. They stay in
`fyai_oauth.c` and in the provider code, as CLAUDE.md states. The component only
sees their result in the store. Its states start at the store.

### 3.2 Scheduled refresh

A holder that lives long (an agent session, the transport, an MCP connection)
asks the source to `schedule`: one timer at expiry minus the window. When it
fires, it starts the shared refresh with no waiter. MCP OAuth does this
already; the ChatGPT login and the transport gain it. The timer is a way to
avoid a parked request, not a requirement: `prepare` covers the case where the
timer did not run.

## 4. States

### 4.1 The stored credential

| State | Test | `prepare` | `reject` |
| --- | --- | --- | --- |
| ABSENT | no record, or the load fails | -ENOENT | -ENOENT |
| SIGNED_OUT | registration only, no access token | -ENOENT | -ENOENT |
| UNAUTHORIZED | token without the plan scope (`credentials_ready` false) | -ESTALE | -ESTALE |
| FRESH | expiry beyond the window | 0 | tag differs: 0; tag equals: refresh |
| DUE | inside the window | refresh | as FRESH |
| EXPIRED | expiry in the past | refresh | as FRESH |
| NO_REFRESH | expiry past and no refresh token | -ESTALE | -ESTALE |
| REPLACED | the tag of the stored token differs from the tag in hand | 0 after a reload | 0, retry with the stored token |
| REVOKED | the provider answers 401 for a FRESH token; the refresh fails with an HTTP error | the refresh result | the refresh result |

A static source has the states ABSENT (unset or empty) and PRESENT, and the
tag of its value. A key that changed in the store or environment is REPLACED.

### 4.2 The refresh

| State | Meaning | Next |
| --- | --- | --- |
| IDLE | none runs | `prepare`, `reject` or the timer start one |
| WAIT_LOCK | another process holds the store lock | 50 ms timer; with the lock, read the store again: if it is FRESH, or its tag differs from the rejected tag, DONE with no network |
| DISCOVER | the signing keys, or the metadata of an MCP server, are fetched | TOKEN or FAILED |
| TOKEN | the token request runs | DONE or FAILED |
| DONE | tokens saved, lock released, tag of the new token recorded | waiters run with 0; IDLE; the timer is armed again |
| FAILED | any error, with the lock released | each waiter runs one time with the error; IDLE; no retry by timer for one backoff period |
| CANCELLED | process exit, or the last waiter left in WAIT_LOCK or DISCOVER | transfer, timer and lock removed |

Rules:

- One refresh for each source in a process. A second caller attaches a waiter.
- A waiter that has waited (`resumed`) never starts a refresh. A failed
  refresh ends each waiter one time with one cause.
- `force` is replaced by the tag: "the token with this tag is rejected".
  Under the lock, the store decides.
- A refresh in TOKEN is not cancelled by the loss of its waiters: the provider
  has seen the refresh token, and a rotated token must reach the store.
- A failed scheduled refresh does not repeat at once. It waits, and each
  `prepare` still answers from the state.

### 4.3 A request

| State | Event | Action |
| --- | --- | --- |
| NEW | source has no `prepare`, or `prepare` = 0 | build the headers, SENT |
| NEW | `prepare` = 1 | PARKED |
| NEW | `prepare` < 0 | fail with the cause; no request without a credential |
| PARKED | waiter, 0 | `prepare(resumed)`, build the headers, SENT |
| PARKED | waiter, error | fail with the cause of the refresh |
| PARKED | cancel | remove the waiter; settle as cancelled |
| SENT | 2xx | finish |
| SENT | 401, not RETRIED, probe yes | keep the response from the user; `reject(tag)` |
| SENT | 401, RETRIED | the 401 is the response |
| SENT | 401, probe no | the 401 is the response |
| REJECTED | `reject` = 0 | build the headers, SENT, mark RETRIED |
| REJECTED | `reject` = 1 | PARKED, mark RETRIED |
| REJECTED | `reject` < 0 | the 401 is the response |
| SENT | transient failure, 429, 5xx | the retry rules of the engine, unchanged; a retry starts at NEW |
| any | the request ends while a waiter is set | remove the waiter first |
| any | transport lost | the error of the transport; no direct request |

RETRIED is a field of the request, as in the transport server. It replaces
`retry.refreshed` of `fyai_stream.c`.

### 4.4 Mode and path

| Path | No isolation | Isolation |
| --- | --- | --- |
| Model request | the agent calls the component | the transport server calls it; the agent has no source |
| Model discovery | the agent, through a waiter | a profile with an egress rule; the transport calls it. Today it is refused. |
| MCP HTTP, static or OAuth | the agent, a source `oauth:mcp:NAME` | open: SDD section 7 says MCP needs an ownership policy. The component does not decide it; it lets the policy name a profile later. |
| MCP stdio | environment grant only | unchanged |
| Sub-agent child | its own process; shares the store and lock; the tag test reloads a token that a sibling refreshed | the transport serves it |
| Tool child | no credential | no credential |
| Verb | blocking wrapper in a standalone command | not isolated (SDD section 5.6) |
| Session or SDK switch | `fyai_auth_resolve()` chooses the source and never refreshes | the same, and the profile |
| Setup, spawn, import | `fyai_auth_resolve()` and a blocking `prepare` in a context that owns the loop | the same |

## 5. Coverage

The component serves the ChatGPT login in the agent (`fyai_xfer.c`, model
discovery and the scheduled refresh) and in the transport, and the static
sources `env:`, `secret:` and `mem:`. `fyai_auth_resolve()` only chooses the
source and never waits. A request that needs a due token waits for the shared
refresh.

- **Cancel:** a cancel in WAIT_LOCK or DISCOVER removes the waiter. A cancel in
  TOKEN detaches the request, because the provider can rotate the refresh
  token when it receives it: the owner is told at once and the end of the
  transfer saves the tokens. At exit, `fyai_auth_drain()` waits for such a
  request for at most 30 s.
- **Timer:** one timer for each process fires at the due time of the token
  (expiry minus 300 s). It reads the store first; a token that another process
  renewed only moves the timer. A failed refresh is tried again after 300 s. A
  token with less than 60 s between now and its due time gets no timer.
- **Stored API key:** a key of the secret store is read again after a 401. A
  key that changed is sent in a second send; the same key leaves the 401 as the
  answer. A key from the environment or from `--api-key` cannot change while
  the process runs, and keeps the single read at setup.
- **MCP:** MCP OAuth is not a registry source. Its discovery, registration and
  authorization belong to the start of one server and stay in `fyai_mcp.c`.
  The refresh is a request of its own (`src/fyai_mcp_oauth.c`): it takes the
  store lock, reads the record again, adopts a token that another process
  renewed, and saves under the lock. A cancel lets a token request that is out
  end; a logout aborts it. A save of the registration or of the tokens waits
  for the store lock on a timer. The code exchange does not compare the record
  before it saves: it is an explicit login and the last one stays. A static
  token is an owned copy, as the access token is.
- **Limits:** a held 401 body is capped at 64 KiB. The transport backend of
  `fyai_xfer` relies on the transport for the login. Model discovery fetches
  from a fixed HTTPS endpoint, so its tests cover the wait, the cancel and the
  failed refresh, not the fetch.

## 6. Tests

The tests serve the provider and the issuer with `fyai_httpd` on the loop of
the test (`tests/fyai_auth_mock.h`). The issuer signs ID tokens, so a refresh
that succeeds is tested. `tests/fyai_cred_test.c` covers the states and the
timer, `tests/fyai_xfer_login_test.c` the request states,
`tests/fyai_auth_cancel_test.c` the cancel paths, and
`tests/fyai_mcp_oauth_test.c` the MCP refresh. The case `mcp_oauth_busy_store`
covers a login that finds the store locked.
