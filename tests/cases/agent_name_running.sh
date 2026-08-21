#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent that is still running cannot be addressed again.
#
# A name reaches one sub-agent and one conversation. A second call under a name
# that is stored carries that sub-agent on, but one that is still running owns
# its conversation, so the call is refused and the model is told why.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_name_running.json

run_fyai --set display/stream=false --set api=chat-completions \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate twice under one name"
assert_status 0

# The refusal reaches the model as a tool result, and says what to do.
assert_any_request "'is still running' in json.dumps(r)"

# One sub-agent ran, and it kept its branch.
run_fyai branch --all
assert_status 0
assert_stdout_contains "main/agent:twin"
[ "$(grep -c 'agent:twin' "$TEST_DIR/stdout")" -eq 1 ] ||
	fail "expected exactly one sub-agent branch"

mock_stop
pass
