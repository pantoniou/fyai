#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a forked sub-agent starts from the conversation that made it.
#
# `context: fork` is the default: the sub-agent branches at the head of the
# branch that started it, thus it sees the road up to the fork. `fresh` starts
# it from the task alone.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_fork.json

# Turn one puts something in the conversation that only the parent knows.
run_fyai --set display/stream=false --set tools=true --set api=responses \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
	 --set 'agent/personas/explore/description=Fork model test.' \
	 --set 'agent/personas/explore/system_prompt=You are the fork tester.' \
	 --set 'agent/personas/explore/model=mock-model-mini' \
	 "remember a pass phrase"
assert_status 0

# Turn two delegates. The request of the sub-agent must carry turn one.
run_fyai --set display/stream=false --set tools=true --set api=responses \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
	 "ask a sub-agent to continue"
assert_status 0

# Match the request *of the sub-agent* - the one that carries the persona as an
# instruction message - and require the conversation of the parent in the same
# request. The request of the parent carries the pass phrase as a matter of
# course, thus a match on any request would prove nothing.
assert_any_request "'fork tester' in json.dumps(r['body'].get('input','')) and 'MAGENTA-KESTREL' in json.dumps(r['body'].get('input',''))"
# A fork keeps the parent's resolved model even when its persona names another.
assert_request 2 'r["body"]["model"] == "mock-model"'

# The fork is recorded on the branch of the sub-agent.
run_fyai branch --all
assert_status 0
assert_stdout_contains "main/agent:explore"

mock_stop 4
pass
