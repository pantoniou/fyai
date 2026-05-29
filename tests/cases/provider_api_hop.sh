#!/bin/bash
# SPDX-License-Identifier: MIT
# Provider hopping with API changes: one conversation crosses three API
# grammars - each with its own endpoint, model and key, selected per hop with
# CLI flags - Responses (tool round trip) -> Chat Completions -> Anthropic
# Messages -> back to Responses. Every hop must replay the whole canonical
# history in the target provider's wire shape, using that provider's model and
# credentials.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start api_hop.json

# Per-hop endpoint/model/grammar/key are supplied entirely on the command line
# (no provider presets): --url + the API-mode flag + -m + -k.
HOP_RESP="--responses -u $MOCK_URL/v1/responses -m model-resp -k key-resp"
HOP_CHAT="--chat-completions -u $MOCK_URL/v1/chat/completions -m model-chat -k key-chat"
HOP_MSGS="--messages -u $MOCK_URL/v1/messages -m model-msgs -k key-msgs"

run_hop() {
	local flags="$1"; shift
	set +e
	# shellcheck disable=SC2086
	"$FYAI_BIN" --color off --no-markdown --no-stream $flags "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
}

printf 'mock data payload\n' > data.txt

# Hop 1: Responses, with a tool round trip (requests 0 and 1).
run_hop "$HOP_RESP" -t "read data.txt"
assert_status 0
assert_stdout_contains "Hop one done under Responses."
assert_request 0 'r["path"] == "/v1/responses"'
assert_request 0 'r["auth"] == "Bearer key-resp"'
assert_request 0 'r["body"]["model"] == "model-resp"'
assert_request 1 'any(i.get("type") == "function_call_output" and i.get("call_id") == "call_hop_1" and "mock data payload" in i.get("output", "") for i in r["body"]["input"])'

# Hop 2: Chat Completions (request 2) - the Responses tool turn must replay
# as assistant tool_calls + role:tool messages.
run_hop "$HOP_CHAT" "hop to chat"
assert_status 0
assert_stdout_contains "Hop two done under Chat Completions."
assert_request 2 'r["path"] == "/v1/chat/completions"'
assert_request 2 'r["auth"] == "Bearer key-chat"'
assert_request 2 'r["body"]["model"] == "model-chat"'
assert_request 2 'any(tc["id"] == "call_hop_1" and tc["function"]["name"] == "read_file" for m in r["body"]["messages"] if m.get("tool_calls") for tc in m["tool_calls"])'
assert_request 2 'any(m.get("role") == "tool" and m.get("tool_call_id") == "call_hop_1" for m in r["body"]["messages"])'
assert_request 2 'any(m.get("content") == "Hop one done under Responses." for m in r["body"]["messages"])'

# Hop 3: Anthropic Messages (request 3) - the same history must arrive as
# alternating-role messages with tool_use/tool_result content blocks and the
# per-provider auth headers.
run_hop "$HOP_MSGS" "hop to messages"
assert_status 0
assert_stdout_contains "Hop three done under Anthropic Messages."
assert_request 3 'r["path"] == "/v1/messages"'
assert_request 3 'r["x_api_key"] == "key-msgs"'
assert_request 3 'r["auth"] == ""'
assert_request 3 'r["body"]["model"] == "model-msgs"'
assert_request 3 'r["body"]["max_tokens"] > 0'
assert_request 3 'any(b.get("type") == "tool_use" and b.get("id") == "call_hop_1" and b.get("input") == {"path": "data.txt"} for m in r["body"]["messages"] if m["role"] == "assistant" for b in m["content"] if isinstance(b, dict))'
assert_request 3 'any(b.get("type") == "tool_result" and b.get("tool_use_id") == "call_hop_1" for m in r["body"]["messages"] if m["role"] == "user" for b in m["content"] if isinstance(b, dict))'
assert_request 3 'any(b.get("type") == "text" and b.get("text") == "Hop two done under Chat Completions." for m in r["body"]["messages"] for b in m["content"] if isinstance(b, dict))'
# roles must strictly alternate for the Messages API
assert_request 3 'all(a["role"] != b["role"] for a, b in zip(r["body"]["messages"], r["body"]["messages"][1:]))'
assert_request 3 'r["body"]["messages"][0]["role"] == "user"'

# Hop 4: back to Responses (request 4) - the turn produced under Messages
# mode must replay cleanly as Responses input.
run_hop "$HOP_RESP" "hop back home"
assert_status 0
assert_stdout_contains "Back home under Responses."
assert_request 4 'r["path"] == "/v1/responses"'
assert_request 4 'r["auth"] == "Bearer key-resp"'
assert_request 4 'any(i.get("type") == "function_call" and i.get("call_id") == "call_hop_1" for i in r["body"]["input"])'
assert_request 4 'any(i.get("content") == "Hop three done under Anthropic Messages." for i in r["body"]["input"] if isinstance(i.get("content"), str))'
assert_request 4 'all(i.get("role") != "system" for i in r["body"]["input"])'

# the display view walks all four turns off the one canonical state
assert_state_contains "Hop one done under Responses." dump state
assert_state_contains "Hop three done under Anthropic Messages." dump state
assert_state_contains "Back home under Responses." dump state

mock_stop 5
pass
