#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent failure must arrive with its reason and its identity. The parent
# relays the child's diagnostics into the tool result, so a bare "sub-agent
# failed" there means the child recorded nothing the user can act on.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_failure_detail.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=true --set retry/max_attempts=1 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate it"
assert_status 0

# The parent's tool result carries what went wrong, not only that it did.
assert_request 2 \
	'any(m.get("role") == "tool" and "HTTP 400" in m.get("content", "") '\
'and "worker" in m.get("content", "") for m in r["body"]["messages"])'

# The provider's own words survive the relay.
assert_request 2 \
	'any(m.get("role") == "tool" and "the request was rejected" in '\
'm.get("content", "") for m in r["body"]["messages"])'

# The user is told as well, marked with the branch the sub-agent ran on: the
# child is another process, so its sink is drained by nobody but the parent.
assert_stderr_contains "[main/agent:worker]"
assert_stderr_contains "the request was rejected"

mock_stop 3
pass
