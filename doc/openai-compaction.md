<!-- SPDX-License-Identifier: MIT -->

# OpenAI Responses compaction

fyai contains two server-side Responses compaction paths. Both return an
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

This is a legacy compatibility path. Its presence in fyai does not establish
support in the public ChatGPT subscription API.

## Provider capability

These items are part of the OpenAI Responses protocol. A compatible provider
can implement them, but fyai must not infer support from a provider name. The
selected catalogue endpoint must declare server-side compaction support.

## ChatGPT subscription access

The direct subscription flow disables both server-side paths, even if the
catalogue declares compaction support. Context compaction uses an ordinary
streamed summarization request instead. It sends the required input to the
public Responses endpoint with `store:false`. See the
[authentication flow](chatgpt-auth.md) and
[OpenAI preview limits](https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations).
