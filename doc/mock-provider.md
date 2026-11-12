# The mock provider

The test suite uses a mock AI provider. It is the hidden command
`fyai __mock-server SCENARIO [RUNDIR]`. It is part of the `fyai` binary and
runs on the event loop of the process. It uses one thread. A reply that waits
does not block another connection.

The mock listens on `127.0.0.1` on a port that the kernel chooses. The HTTP
side is `fyai_httpd` (`src/fyai_httpd.h`): it has no TLS, it keeps the
connection (keep-alive), and it reads a request body with a `Content-Length` or
with chunked encoding, up to 64 MiB. It answers `Expect: 100-continue` and
takes HTTP/1.0. A request head is at most 8 KiB. The mock accepts `POST`, `GET`
and `DELETE`. Any other method gets status 501. The mock writes each reply
itself with `fyai_httpd_conn_write()` and ends it with
`fyai_httpd_conn_done()`, so a reply can hold, split or cut its bytes.

## Run files

The mock writes these files in RUNDIR (default: the current directory):

| File | Content |
|---|---|
| `port` | The port, written when the mock listens. The file appears whole. |
| `requests.jsonl` | One JSON object for each request. |
| `served` | The number of requests that picked a step. |
| `stream-wait-failed` | Created when a wait for a marker file expired. |

A `POST` record has `path`, `auth`, `x_api_key`, `anthropic_version`,
`mcp_session_id`, `mcp_protocol_version`, `client_port`, `content_type` and
`body`. The body is the parsed JSON, or the text when it is not JSON. A `GET`
or `DELETE` record has `path`, `method`, `auth`, `client_port` and an empty
`body`. A `DELETE` record also has the two MCP fields.

The process ends when it gets SIGTERM. With `--log`, it reports one line for
each request on standard error. The test harness passes `--log` when
`FYAI_MOCK_LOG` is set, and the lines go to `mock.log` in the run directory.

## Scenario

A scenario is a JSON or YAML file with a `steps` sequence. A step describes the
reply to one request. A request takes the next step in order. If any step has
`match`, a request takes the first unused step whose `match` fits it. When no
step is left, the reply is status 500 with `mock: scenario exhausted`.

| Key | Meaning |
|---|---|
| `status` | HTTP status. The default is 200. |
| `response` | A JSON value for the body. |
| `raw` | The body as given. It can be invalid JSON. |
| `headers` | Extra headers of a `POST` or `GET` reply. `{{BASE_URL}}` in a value is the URL of the mock. |
| `sse` | A sequence of events. Each is `data: JSON`. A mapping with `type` also has an `event:` line. |
| `raw_sse` | A sequence of lines for the stream, each followed by a newline. |
| `done` | Add `data: [DONE]` at the end of the stream. |
| `chunk_split` | Byte offsets in the stream where a new write starts. |
| `chunk_delay` | Seconds to wait between the writes of `chunk_split`. |
| `delay` | Seconds to wait before the reply starts. |
| `wait_for` | The name of a marker file in RUNDIR. Without `wait_after_event`, the reply starts when the file exists. |
| `wait_after_event` | With `wait_for`, the stream stops after this event (zero-based) until the file exists. |
| `close_after` | Send only this number of `sse` events. The reply promises 64 more bytes than it sends. Then the mock closes the connection. |
| `match` | See below. |

A wait for a marker ends after 30 seconds multiplied by `FYAI_TIMEOUT_SCALE`
before a reply, and after 5 seconds inside a stream. When the wait expires, the
mock creates `stream-wait-failed` and goes on.

A `GET` reply is `raw` or `response`, with `{{BASE_URL}}` replaced. A `DELETE`
reply is `raw`. `wait_for` and `delay` apply to `POST` only.

### match

`match` is a mapping. Every key must fit. The keys `http` (the method), `path`,
`method` (the `method` of a JSON-RPC body), `auth` (the `Authorization`
header) and `session` (the `Mcp-Session-Id` header) must be equal to the value.
The key `body_contains` is a substring test on the request body. An unknown key
fits only the empty string.
