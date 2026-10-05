#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a session in a view can start a sub-agent: the sub-agent shares the
# view, so its change stays out of the project, and every request of the session
# and of the sub-agent reaches the provider. Under credential isolation the
# session and the sub-agent are executions of the transport, and the transport
# names each by a pidfd, because their PIDs are those of a PID namespace of the
# view.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view must lie outside the project.
export TMPDIR="$FYAI_TMPDIR_BASE"
printf baseline > file
mock_start session_isolated_agent.json

run_fyai --set view/isolate_session=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate an edit"
assert_status 0
assert_stdout_contains "Session and sub-agent done."
assert_stderr_contains "ran in the view 'session'"

# The sub-agent worked in the view of the session.
[ "$(cat file)" = baseline ] || fail 'the sub-agent changed the project'

# All four requests got a key at the provider, from the session or its sub-agent.
assert_request 0 'r["auth"] == "Bearer test-key"'
assert_request 1 'r["auth"] == "Bearer test-key"'
assert_request 2 'r["auth"] == "Bearer test-key"'
assert_request 3 'r["auth"] == "Bearer test-key"'

mock_stop 4
pass
