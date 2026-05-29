#!/bin/bash
# SPDX-License-Identifier: MIT
# Responses tool round trip: function_call item answered by
# function_call_output with the matching call_id.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_tools_read_file.json

printf 'mock data payload\n' > data.txt

run_fyai --responses --no-stream -t -u "$MOCK_URL/v1/responses" \
	 -m mock-model "read data.txt"
assert_status 0
assert_stdout_contains "The file says: mock data payload."

# first request advertises flat Responses-style function tools
assert_request 0 'any(t.get("type") == "function" and t.get("name") == "read_file" for t in r["body"]["tools"])'

# second request replays the call and answers it
assert_request 1 'any(i.get("type") == "function_call" and i.get("call_id") == "call_read_1" for i in r["body"]["input"])'
assert_request 1 'any(i.get("type") == "function_call_output" and i.get("call_id") == "call_read_1" and "mock data payload" in i.get("output", "") for i in r["body"]["input"])'

mock_stop 2
pass
