#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify recursive agent question routing through the root owner.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_recursive_question.json
run_fyai --set display/stream=false --set tools=true --set api=responses \
	--set api_url="$MOCK_URL/v1/responses" -m mock-model \
	--answer green "delegate recursively"
assert_status 0
assert_stdout_contains "Recursive delegation complete."
assert_stderr_contains "NESTED_COLOUR?"
assert_request 3 '"green" in json.dumps(r["body"])'
mock_stop 6
pass
