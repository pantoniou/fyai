#!/bin/bash
# SPDX-License-Identifier: MIT
# Streaming tool-call deltas: name and arguments split across SSE chunks
# must reassemble into one read_file call, then a second streamed answer.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_stream_tool_deltas.json

printf 'delta payload\n' > data.txt

run_fyai --set api=chat-completions --set tools=true --set api_url="$MOCK_URL/v1/chat/completions" \
	 -m mock-model "read data.txt"
assert_status 0
assert_stdout_contains "Tool deltas assembled fine."

# the reassembled call round-trips with the full name/arguments
assert_request 1 'any(tc["function"]["name"] == "read_file" and tc["function"]["arguments"] == "{\"path\":\"data.txt\"}" for m in r["body"]["messages"] if m.get("tool_calls") for tc in m["tool_calls"])'
assert_request 1 'any(m.get("role") == "tool" and m.get("tool_call_id") == "call_delta_1" and "delta payload" in m.get("content", "") for m in r["body"]["messages"])'

mock_stop 2
pass
