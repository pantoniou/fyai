#!/bin/bash
# SPDX-License-Identifier: MIT
# A delegation requests a time limit.
#
# The call limit has priority over the persona limit and agent/timeout_ms.
# The model supplies the call limit, so agent/max_timeout_ms bounds it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_call_timeout.json

# agent/timeout_ms disables the default limit. The turn ends only if the
# requested limit of 1500 ms is active.
run_fyai --set display/stream=false --set agent/timeout_ms=0 \
	 --set api=responses --set api_url="$MOCK_URL/v1/responses" \
	 -m mock-model "delegate a task that does not end"
assert_status 0
assert_stdout_contains "The sub-agent was stopped."
assert_any_request "'timed out after 1500 ms' in json.dumps(r)"

mock_stop 3
pass
