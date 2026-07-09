#!/bin/bash
# SPDX-License-Identifier: MIT
# Chat Completions tool round trip: read_file call, tool result replayed.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_tools_read_file.json

run_fyai --set api=chat-completions --no-stream -t \
	 -u "$MOCK_URL/v1/chat/completions" -m mock-model "read missing.py"
assert_status 0
assert_stdout_contains "Could not read missing.py."

# first request advertises the function tools
assert_request 0 'any(t["function"]["name"] == "read_file" for t in r["body"]["tools"])'
assert_request 0 'r["body"]["tool_choice"] == "auto"'

# second request replays the assistant tool call and carries the tool result
assert_request 1 'any(m.get("role") == "assistant" and m.get("tool_calls") for m in r["body"]["messages"])'
assert_request 1 'any(m.get("role") == "tool" and m.get("tool_call_id") == "call_read_1" and "tool error:" in m.get("content", "") for m in r["body"]["messages"])'

mock_stop 2

run_fyai history --raw
assert_status 0
assert_stdout_contains "tool error:"
assert_stdout_contains '```'
assert_stdout_not_contains '```python'
pass
