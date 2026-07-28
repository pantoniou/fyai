#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a sub-agent keeps its conversation on its own branch.
#
# The name of the sub-agent comes from the model, thus it is arbitrary text and
# is namespaced below the branch that started it. The sub-agent runs in a forked
# child over a shared arena mapping, thus it re-opens the arena to become an
# independent writer before it publishes.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_tool_responses.json

run_fyai --set display/stream=false --set api=responses \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
	 "delegate a greeting to a sub-agent"
assert_status 0
assert_stdout_contains "Delegated and done."

# The model called the sub-agent "greeter", thus that is the name of its branch,
# below the branch that started it. The "agent:" marker cannot come from a user,
# because ':' is not valid in a name that a user gives.
run_fyai branch --all
assert_status 0
assert_stdout_contains "main/agent:greeter"

# The conversation of the sub-agent is kept, not thrown away with the process.
run_fyai --branch main/agent:greeter transcript
assert_status 0
assert_stdout_contains "print a greeting with the shell and report back"
assert_stdout_contains "SUBAGENT_REPORT"

# The parent branch is untouched by the publish of the sub-agent: both survive,
# which is what the CAS between the two writers has to guarantee.
run_fyai transcript
assert_status 0
assert_stdout_contains "Delegated and done."

mock_stop 4
pass
