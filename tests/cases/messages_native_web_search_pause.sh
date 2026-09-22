#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start messages_native_web_search_pause.json

run_fyai --set api=messages --set display/stream=false --set tools=false \
	--set web_search=true --set api_url="$MOCK_URL/v1/messages" \
	-m mock-model "check current fyai status"
assert_status 0
assert_stdout_contains "after continuation"
assert_request 1 'any(b.get("type") == "server_tool_use" for m in r["body"]["messages"] if m["role"] == "assistant" for b in m["content"])'
assert_request 1 'any(b.get("type") == "web_search_tool_result" for m in r["body"]["messages"] if m["role"] == "assistant" for b in m["content"])'

mock_stop 2
pass
