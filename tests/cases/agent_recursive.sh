#!/bin/bash
# SPDX-License-Identifier: MIT
# A child can delegate and collect a durable grandchild conversation.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_recursive.json

run_fyai --set display/stream=false --set tools=true --set api=responses \
	--set api_url="$MOCK_URL/v1/responses" -m mock-model "delegate recursively"
assert_status 0
assert_stdout_contains "Recursive delegation complete."
assert_request 1 'any(t.get("name") == "agent" for t in r["body"].get("tools", []))'
assert_request 3 '"NESTED_REPORT" in json.dumps(r["body"])'

run_fyai --set display/markdown=false branch --all
assert_status 0
assert_stdout_contains "main/agent:child"
run_fyai -b main/agent:child/agent:grandchild transcript
assert_status 0
assert_stdout_contains "NESTED_REPORT"
mock_stop 5
pass
