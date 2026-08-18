#!/bin/bash
# SPDX-License-Identifier: MIT
# A large parallel agent group publishes from every child at once, and the
# parent publishes its own turn on top. The state race must settle: the run
# has to end with the turn stored, not with a lost publish.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agents_many.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate everything"
assert_status 0
assert_stdout_contains "All agents reported."
assert_stderr_not_contains "lost the state race"
assert_stderr_not_contains "could not publish"

# The turn is durable: a later invocation sees it.
run_fyai transcript --raw
assert_status 0
assert_stdout_contains "All agents reported."

# Every sub-agent kept its own branch.
run_fyai branch --all
assert_status 0
assert_stdout_contains "main/agent:ag0"
assert_stdout_contains "main/agent:ag7"

# The reflog stays walkable and the parent branch keeps its own turn, after the
# sub-agent publishes advanced the root under it.
run_fyai list reflog --brief
assert_status 0
run_fyai --branch main transcript --raw
assert_status 0
assert_stdout_contains "delegate everything"

mock_stop 10
pass
