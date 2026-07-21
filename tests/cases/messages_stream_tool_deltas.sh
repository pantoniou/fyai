#!/bin/bash
# SPDX-License-Identifier: MIT
# Messages streaming tool use: input_json_delta fragments split across SSE
# chunks must reassemble into one read_file call, then a second streamed
# answer completes the turn.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start messages_stream_tool_deltas.json

printf 'delta payload\n' > data.txt

run_fyai --set api=messages --set tools=true --set api_url="$MOCK_URL/v1/messages" -m mock-model "read data.txt"
assert_status 0
assert_stdout_contains "Tool deltas assembled fine."

# the reassembled call round-trips with the full input
assert_request 1 'any(b.get("type") == "tool_use" and b.get("id") == "toolu_delta_1" and b.get("input") == {"path": "data.txt"} for m in r["body"]["messages"] if m["role"] == "assistant" for b in m["content"])'
assert_request 1 'any(b.get("type") == "tool_result" and b.get("tool_use_id") == "toolu_delta_1" and "delta payload" in b.get("content", "") for m in r["body"]["messages"] if m["role"] == "user" for b in m["content"] if isinstance(b, dict))'

mock_stop 2
pass
