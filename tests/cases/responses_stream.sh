#!/bin/bash
# SPDX-License-Identifier: MIT
# Responses SSE streaming: output_text deltas + response.completed doc.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_stream.json

run_fyai --set logging/stream true --set api=responses -u "$MOCK_URL/v1/responses" -m mock-model \
	 --stats "stream please"
assert_status 0
assert_stdout_contains "Streamed responses answer."
test -s .fyai/logs/stream.yaml || fail "missing stream log"
grep -q 'response.output_text.delta' .fyai/logs/stream.yaml || fail "stream log missing delta"

assert_request 0 'r["body"]["stream"] is True'

# usage comes from the completed response document
assert_stderr_contains "input=15"

# the canonical state records the completed doc's message
assert_state_contains "Streamed responses answer." dump state

mock_stop 1
pass
