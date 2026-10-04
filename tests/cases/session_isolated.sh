#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that view/isolate_session runs a prompt in a view of the project: the
# model works on the files there, the project stays as it was, and the view
# holds the changes.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
# A session in a view cannot hold the credentials apart: the run refuses it.
[ -z "${FYAI_TEST_TRANSPORT:-}" ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view must lie outside the project.
export TMPDIR="$FYAI_TMPDIR_BASE"
printf baseline > file
mock_start session_isolated.json

run_fyai --set view/isolate_session=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "edit the files"
assert_status 0
assert_stdout_contains "Session edit done."
assert_stderr_contains "ran in the view 'session'"
assert_stderr_contains "2 paths changed:"

[ "$(cat file)" = baseline ] || fail 'the isolated session changed the project'
[ ! -e added ] || fail 'the isolated session added a file to the project'

run_fyai view diff session --stat
assert_status 0
assert_stdout_contains 'file'
assert_stdout_contains 'added'

# The project takes the result when the user applies it.
run_fyai view apply session
assert_status 0
[ "$(cat file)" = changed ] || fail 'apply did not write the file'
[ "$(cat added)" = new ] || fail 'apply did not write the added file'

mock_stop 2
pass
