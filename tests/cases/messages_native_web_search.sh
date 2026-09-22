#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start messages_native_web_search.json

run_fyai --set api=messages --set display/stream=false --set tools=false \
	--set web_search=true --set api_url="$MOCK_URL/v1/messages" \
	-m mock-model "check current fyai status"
assert_status 0
assert_stdout_contains "Fyai is current"
assert_request 0 'any(t.get("type") == "web_search_20250305" and t.get("name") == "web_search" for t in r["body"]["tools"])'

mock_stop 1
pass
