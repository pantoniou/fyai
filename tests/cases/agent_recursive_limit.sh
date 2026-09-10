#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify recursive agent limit routing through the root owner.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_recursive_limit.json
run_fyai --set display/stream=false --set tools=true --set api=responses \
	--set api_url="$MOCK_URL/v1/responses" -m mock-model \
	--set agent/max_live_agents=1 "delegate recursively"
assert_status 0
assert_stdout_contains "Recursive delegation complete."
assert_request 2 '"admission refused" in json.dumps(r["body"])'
mock_stop 4
pass
