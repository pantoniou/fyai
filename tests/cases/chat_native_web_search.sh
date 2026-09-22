#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_native_web_search.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=false \
	--set web_search=true --set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "check current fyai status"
assert_status 0
assert_stdout_contains "Fyai is current"
assert_request 0 '"web_search_options" in r["body"]'
assert_request 0 '"tools" not in r["body"]'

mock_stop 1
pass
