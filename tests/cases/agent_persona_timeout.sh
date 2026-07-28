#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a persona overrides the default agent time limit.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_persona_timeout.json

run_fyai --set display/stream=false --set agent/timeout_ms=30000 \
	 --set agent/personas/stubborn/timeout_ms=1500 \
	 --set api=responses --set api_url="$MOCK_URL/v1/responses" \
	 -m mock-model "delegate a task that does not end"
assert_status 0
assert_stdout_contains "The sub-agent was stopped."
assert_any_request "'timed out after 1500 ms' in json.dumps(r)"

mock_stop 3
pass
