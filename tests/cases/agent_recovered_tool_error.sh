#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent that recovers from a rejected tool call has completed its work.
#
# A tool call the model is told about is answered by the tool result: the model
# reads the reason and calls again. Such a report must not fail the sub-agent
# that recovered from it, and must not stand as the cause of the run, where it
# hides whatever fails later. The user still receives the reason.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_recovered_tool_error.json

# The sub-agent asks for a shell session with no terminal, is told why, and
# then reports. The parent must receive that report.
run_fyai --set tools=true --set builtin_shell=true --set display/stream=false \
	--set api=chat-completions --set "api_url=$MOCK_URL/v1/chat/completions" \
	-m mock-model "delegate the job"
assert_status 0
assert_stdout_contains "PARENT-SAW-THE-REPORT"

# The reason reached the user, below error severity and named by its branch.
assert_stderr_contains "warning: [main/agent:worker] a session was asked for with no terminal"
assert_stderr_not_contains "sub-agent 'worker' failed"

# The sub-agent's report, not a failure, was returned to the model.
assert_any_request '"RECOVERED-AND-DONE" in json.dumps(r["body"])'
assert_no_request '"sub-agent" in json.dumps(r["body"]) and "failed" in json.dumps(r["body"])'

mock_stop 4
pass
