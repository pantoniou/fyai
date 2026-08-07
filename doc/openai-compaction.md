<!-- SPDX-License-Identifier: MIT -->

# OpenAI Responses compaction

OpenAI has two server-side compaction protocols. Both protocols return an
opaque `compaction` item that replaces the earlier Responses input when the
client continues the conversation.

## Version 1

The original protocol sends the conversation to:

```text
POST /v1/responses/compact
```

The response is one JSON document with an `output` sequence. The client stores
that sequence and sends it as input to the next Responses request.

## Version 2

The newer protocol uses the normal streaming Responses endpoint:

```text
POST /v1/responses
```

The client appends this item to the request input:

```json
{"type":"compaction_trigger"}
```

The stream must complete with one output item whose type is `compaction`. The
client stores that item and sends it as input to the next Responses request.

Current Codex releases use version 2 for ChatGPT subscription authentication.
They retain version 1 as a legacy path. This behavior does not, by itself,
mean that OpenAI has formally deprecated the version 1 endpoint.

## Provider capability

These items are part of the OpenAI Responses protocol. A compatible provider
can implement them, but fyai must not infer support from a provider name. The
selected catalogue endpoint must declare server-side compaction support.
