#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify what happens when a sub-agent name is already taken.
#
# The name of the agent is the name of its branch, thus a name that is taken is
# a clash. The spawn fails and the model is told why, so that it can retry with
# a different name; the first sub-agent keeps its branch.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_name_clash.json

run_fyai --set display/stream=false --set api=responses \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
	 "delegate the same task twice"
assert_status 0

# The second delegation under the same name is refused, and the reason reaches
# the model as the tool result rather than ending the turn.
assert_any_request "'already exists on this branch' in json.dumps(r)"

# Exactly one sub-agent branch, from the first call.
run_fyai branch --all
assert_status 0
assert_stdout_contains "main/agent:greeter"
[ "$(grep -c 'agent:greeter' "$TEST_DIR/stdout")" -eq 1 ] ||
	fail "expected exactly one sub-agent branch"

mock_stop
pass
