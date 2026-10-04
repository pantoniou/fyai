#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that an isolated sub-agent works in a view of the project: its writes
# stay in the view, the parent is told where they are, and the arena is
# writable for the sub-agent.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
# A sub-agent in a view cannot be admitted by the credential transport.
[ -z "${FYAI_TEST_TRANSPORT:-}" ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view must lie outside the project.
export TMPDIR="$FYAI_TMPDIR_BASE"
printf baseline > file
mock_start agent_isolated.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate an edit to an isolated sub-agent"
assert_status 0
assert_stdout_contains "Isolated and done."

# The model is told that isolation is optional, and is given the tool to pull.
assert_request 0 'any(t["function"]["name"] == "agent" and "set `isolated` to true" in t["function"]["description"] and "{{" not in t["function"]["description"] for t in r["body"]["tools"])'
assert_request 0 'any(t["function"]["name"] == "project_view" for t in r["body"]["tools"])'
assert_request 1 'not any(t["function"]["name"] == "project_view" for t in r["body"]["tools"])'

# The parent pulled the changes with the project_view tool, and only then.
assert_request 3 'any(m.get("role") == "tool" and "view '"'"'agent-worker'"'"'" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 4 'any(m.get("role") == "tool" and m.get("tool_call_id") == "call_view_1" and isinstance(m.get("content"), dict) and m["content"].get("applied") == 2 for m in r["body"]["messages"])'
[ "$(cat file)" = changed ] || fail 'the tool did not apply the change'
[ "$(cat added)" = new ] || fail 'the tool did not apply the added file'
printf baseline > file
rm added

# The parent is told which view holds the changes, and what they are.
assert_request 3 'any(m.get("role") == "tool" and "view '"'"'agent-worker'"'"'" in m.get("content", "") and "2 paths changed:" in m.get("content", "") for m in r["body"]["messages"])'

# The view records the result.
run_fyai view diff agent-worker --stat
assert_status 0
assert_stdout_contains 'file'
assert_stdout_contains 'added'

# A dry run reports the writes and makes none.
run_fyai view apply --dry-run agent-worker
assert_status 0
assert_stdout_contains 'would write'
[ "$(cat file)" = baseline ] || fail 'a dry run changed the project'
[ ! -e added ] || fail 'a dry run added a file'

# A path that the project changed since the baseline is a conflict, and is
# left alone. The other paths are applied.
printf host > file
run_fyai view apply agent-worker
assert_status 1
assert_stdout_contains 'action: conflict'
[ "$(cat file)" = host ] || fail 'a conflict changed the project'
[ "$(cat added)" = new ] || fail 'the added file was not applied'
rm added

# A selected path applies alone, once the project is at the baseline again.
printf baseline > file
run_fyai view apply agent-worker file
assert_status 0
[ "$(cat file)" = changed ] || fail 'the selected path was not applied'
[ ! -e added ] || fail 'an unselected path was applied'

# The view keeps what was applied from it.
run_fyai view list --full
assert_status 0
assert_stdout_contains 'applied:'

mock_stop 5
pass
