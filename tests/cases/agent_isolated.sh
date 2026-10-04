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

# The sub-agent changed the files in its view and not in the project.
[ "$(cat file)" = baseline ] || fail 'the isolated sub-agent changed the project'
[ ! -e added ] || fail 'the isolated sub-agent added a file to the project'

# The parent is told which view holds the changes, and what they are.
assert_request 3 'any(m.get("role") == "tool" and "view '"'"'agent-worker'"'"'" in m.get("content", "") and "2 paths changed:" in m.get("content", "") for m in r["body"]["messages"])'

# The view records the result.
run_fyai view diff agent-worker --stat
assert_status 0
assert_stdout_contains 'file'
assert_stdout_contains 'added'

mock_stop 4
pass
