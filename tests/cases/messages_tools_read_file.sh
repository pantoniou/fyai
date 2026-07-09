#!/bin/bash
# SPDX-License-Identifier: MIT
# Messages tool round trip: a tool_use block executes read_file and is
# answered with a user tool_result carrying the matching tool_use_id; the
# replayed history keeps the assistant tool_use block.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start messages_tools_read_file.json

printf 'mock data payload\n' > data.txt

run_fyai --set api=messages --set display/stream=false -t -u "$MOCK_URL/v1/messages" \
	 -m mock-model "read data.txt"
assert_status 0
assert_stdout_contains "The file says: mock data payload."

# first request advertises flat Anthropic tools (input_schema, no wrapper)
assert_request 0 'any(t.get("name") == "read_file" and "input_schema" in t for t in r["body"]["tools"])'
assert_request 0 'r["body"]["tool_choice"] == {"type": "auto"}'

# second request replays the tool_use block and answers with tool_result
assert_request 1 'any(b.get("type") == "tool_use" and b.get("id") == "toolu_read_1" and b.get("input") == {"path": "data.txt"} for m in r["body"]["messages"] if m["role"] == "assistant" for b in m["content"])'
assert_request 1 'any(b.get("type") == "tool_result" and b.get("tool_use_id") == "toolu_read_1" and "mock data payload" in b.get("content", "") for m in r["body"]["messages"] if m["role"] == "user" for b in m["content"] if isinstance(b, dict))'

mock_stop 2
pass
