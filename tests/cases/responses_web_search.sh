#!/bin/bash
# SPDX-License-Identifier: MIT
# A Responses web_search_call that the provider ran is shown as a tool
# title row and is stored for replay.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_web_search.json

run_fyai --set api=responses --set api_url="$MOCK_URL/v1/responses" \
	 -m mock-model "search please"
assert_status 0
assert_stdout_contains "Found the notes."
assert_stderr_contains "web_search libfyaml release notes"
assert_stderr_contains "libfyaml release notes"

# the stored display document keeps the title row for replay
assert_state_contains "libfyaml release notes" dump anchors
assert_state_contains "tool_head" dump anchors

mock_stop 1
pass
