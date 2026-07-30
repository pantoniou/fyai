#!/bin/bash
# SPDX-License-Identifier: MIT
# Preserve streamed items when response.completed carries an empty output.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_stream_empty_output.json

run_fyai --set api=responses --set api_url="$MOCK_URL/v1/responses" \
	-m mock-model "delegate through the streamed response"
assert_status 0
assert_stdout_contains "Streamed delegation done."

# The branch table wraps at the terminal width, so ask the branch itself for
# its name instead of matching a wrapped cell.
run_fyai branch show main/agent:streamer
assert_status 0
assert_stdout_contains "main/agent:streamer"

run_fyai --branch main/agent:streamer transcript
assert_status 0
assert_stdout_contains "STREAMED_AGENT_REPORT"

mock_stop 3
pass
