#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the time limit of an agent job.
#
# An agent call is a job, thus it gets the same limit machinery as a shell call,
# through agent/timeout_ms. The sub-agent here starts a command that does not
# end, thus the turn completes only if the limit stops the job.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_timeout.json

run_fyai --set display/stream=false --set agent/timeout_ms=1500 \
	 --set api=responses --set api_url="$MOCK_URL/v1/responses" \
	 -m mock-model "delegate a task that does not end"
assert_status 0
assert_stdout_contains "The sub-agent was stopped."

# The model must be told why the sub-agent stopped. From inside the job child a
# group terminate looks the same as an interrupt, thus an unqualified
# "interrupted" here is the regression.
assert_any_request "'timed out after 1500 ms' in json.dumps(r)"

mock_stop 3
pass
