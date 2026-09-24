#!/bin/bash
# SPDX-License-Identifier: MIT
# Messages server_tool_use web searches are shown as tool title rows with
# their outcome, and are not run as local tool calls.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start messages_web_search.json

run_fyai --set api=messages --set api_url="$MOCK_URL/v1/messages" \
	 -m mock-model "search please"
assert_status 0
assert_stdout_contains "Found the notes."
assert_stderr_contains "libfyaml release notes"
assert_stderr_contains "web_search second try (failed)"

assert_state_contains "libfyaml release notes" dump anchors
assert_state_contains "max_uses_exceeded" dump anchors

# one request: the searches are not answered as local tool calls
mock_stop 1
pass
