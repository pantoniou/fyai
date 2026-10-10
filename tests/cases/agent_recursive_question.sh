#!/bin/bash
# SPDX-License-Identifier: MIT
# A grandchild cannot bypass its parent to ask the user.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_recursive_question.json
run_fyai --set display/stream=false --set tools=true --set api=responses \
	--set api_url="$MOCK_URL/v1/responses" -m mock-model \
	"delegate recursively"
assert_status 0
assert_stdout_contains "Recursive delegation complete."
assert_stderr_not_contains "1) red"
assert_request 2 '"ask_user" not in [t.get("name") for t in r["body"]["tools"]] and "ask_parent" in [t.get("name") for t in r["body"]["tools"]]'
assert_request 3 '"ask_user is available only in the root session" in json.dumps(r["body"])'
mock_stop 6
pass
