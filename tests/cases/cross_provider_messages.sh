#!/bin/bash
# SPDX-License-Identifier: MIT
# Cross-provider continuation through the canonical state: a tool-use turn
# made under one API replays correctly when the conversation continues under
# the other (Messages -> Chat Completions, then the reverse).
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# --- Messages first, continue under Chat Completions ---
mock_start cross_messages_then_chat.json
printf 'mock data payload\n' > data.txt

run_fyai --set api=messages --no-stream -t -u "$MOCK_URL/v1/messages" \
	 -m mock-model "read data.txt"
assert_status 0
assert_stdout_contains "Read it under Anthropic."

run_fyai --set api=chat-completions --no-stream -u "$MOCK_URL/v1/chat/completions" \
	 -m mock-model "follow up"
assert_status 0
assert_stdout_contains "Continued under Chat Completions."

# the Anthropic tool turn replays in Chat shapes
assert_request 2 'any(tc["id"] == "toolu_x1" and tc["function"]["name"] == "read_file" for m in r["body"]["messages"] if m.get("tool_calls") for tc in m["tool_calls"])'
assert_request 2 'any(m.get("role") == "tool" and m.get("tool_call_id") == "toolu_x1" and "mock data payload" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 2 'any(m.get("role") == "assistant" and m.get("content") == "Read it under Anthropic." for m in r["body"]["messages"])'
mock_stop 3

# --- Chat Completions first, continue under Messages ---
mock_start cross_chat_then_messages.json

run_fyai --set api=chat-completions --no-stream -t \
	 -u "$MOCK_URL/v1/chat/completions" -m mock-model --new "read data.txt"
assert_status 0
assert_stdout_contains "Read it under Chat Completions."

run_fyai --set api=messages --no-stream -u "$MOCK_URL/v1/messages" \
	 -m mock-model "follow up"
assert_status 0
assert_stdout_contains "Continued under Anthropic."

# the Chat tool turn replays as Anthropic content blocks
assert_request 2 'any(b.get("type") == "tool_use" and b.get("id") == "call_y1" and b.get("input") == {"path": "data.txt"} for m in r["body"]["messages"] if m["role"] == "assistant" for b in m["content"] if isinstance(b, dict))'
assert_request 2 'any(b.get("type") == "tool_result" and b.get("tool_use_id") == "call_y1" and "mock data payload" in b.get("content", "") for m in r["body"]["messages"] if m["role"] == "user" for b in m["content"] if isinstance(b, dict))'
mock_stop 3

pass
