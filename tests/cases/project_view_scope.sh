#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the scope of the names of sub-agents: a user cannot make a view or a
# branch in their namespace, and the project_view tool reaches nothing outside it.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
[ -z "${FYAI_TEST_TRANSPORT:-}" ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
export TMPDIR="$FYAI_TMPDIR_BASE"
printf baseline > file

# The names of views of sub-agents are not for a user.
run_fyai view create uview .
assert_status 0
# The pattern of a name has no separator, and the root of the namespace is reserved.
run_fyai view create agent .
assert_status 1
assert_stderr_contains 'reserved for sub-agent views'
for name in agent/worker agent/a/b; do
	run_fyai view create "$name" .
	assert_status 1
done
run_fyai view list
assert_stdout_not_contains 'agent'

# Nor is the namespace of their branches: a name selects only a branch that a
# sub-agent made, and creates none.
mock_start project_view_scope.json
for branch in main/agent:fake agent:fake; do
	run_fyai --branch "$branch" --set api=chat-completions --set display/stream=false \
		 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
	assert_status 1
	assert_stderr_contains 'namespace of sub-agent branches'
done
FYAI_BRANCH=main/agent:fake run_fyai --set api=chat-completions --set display/stream=false \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status 1
run_fyai branch list
assert_stdout_not_contains 'agent:'

# The tool lists and reads only the views of the sub-agents of the caller.
run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set view/track_project=true --set api_url="$MOCK_URL/v1/chat/completions" \
	 -m mock-model "look at the views"
assert_status 0
assert_stdout_contains 'Scope checked.'
assert_request 1 'any(m.get("role") == "tool" and m.get("tool_call_id") == "list" and m["content"] == [] for m in r["body"]["messages"])'
for id in user branch ref prefixed dots; do
	assert_request 6 'any(m.get("role") == "tool" and m.get("tool_call_id") == "'$id'" and str(m["content"]).startswith("tool error:") for m in r["body"]["messages"])'
done
assert_request 2 'any(m.get("tool_call_id") == "user" and "no sub-agent" in m["content"] for m in r["body"]["messages"])'

mock_stop 7
pass
