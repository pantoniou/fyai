#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that isolated sub-agents run side by side, each in a view of its own.
# Both edit one file, so the second pull is a conflict that the parent reads;
# no agent sees the edit of the other, and the project changes only by a pull.
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
printf baseline > shared
mock_start agents_isolated_parallel.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate both edits"
assert_status 0
assert_stdout_contains "Both pulled."

# Each agent has its own view, and the project took what the first pull held.
[ "$(cat alpha)" = alpha ] || fail 'the first view was not applied'
[ "$(cat beta)" = beta ] || fail 'the second view was not applied'
case "$(cat shared)" in
alpha-edit | beta-edit) ;;
*) fail 'the shared file has a value that no agent wrote' ;;
esac
# The second pull found the file changed and left it alone.
assert_request 6 'any(m.get("tool_call_id") in ("call_view_alpha", "call_view_beta") and isinstance(m.get("content"), str) and json.loads(m["content"]).get("conflicts") == 1 for m in r["body"]["messages"])'

run_fyai view list
assert_status 0
assert_stdout_contains 'agent/alpha'
assert_stdout_contains 'agent/beta'

mock_stop 7
pass
