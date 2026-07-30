#!/bin/bash
# SPDX-License-Identifier: MIT
# The configuration limits the time that a delegation can request.
#
# The model supplies the call `timeout`, so agent/max_timeout_ms bounds it.
# This rule is equivalent to shell/max_timeout_ms for the shell tool. The
# maximum does not apply to operator-configured persona and default limits.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_call_timeout.json

# The call requests 1500 ms, and the configured maximum is 800 ms.
run_fyai --set display/stream=false --set agent/timeout_ms=0 \
	 --set agent/max_timeout_ms=800 \
	 --set api=responses --set api_url="$MOCK_URL/v1/responses" \
	 -m mock-model "delegate a task that does not end"
assert_status 0
assert_stdout_contains "The sub-agent was stopped."
assert_any_request "'timed out after 800 ms' in json.dumps(r)"

mock_stop 3
pass
