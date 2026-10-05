#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that an interactive session runs in a view when view/isolate_session
# asks for it: the terminal belongs to the session in the view, the project
# stays as it was, and the view holds the changes.
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

driver=0
FYAI_PTY_INPUT="edit the files" FYAI_PTY_NEEDLE="Session edit done." FYAI_PTY_TIMEOUT=60 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set view/isolate_session=true \
    --set display/stream=false --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i || driver=$?
if [ "$driver" -ne 0 ]; then
	tail -c 2000 "$TEST_DIR/pty.out" >&2
	fail "the isolated session did not finish"
fi

[ "$(cat file)" = baseline ] || fail 'the isolated session changed the project'
[ ! -e added ] || fail 'the isolated session added a file to the project'
# The notice names the branch that holds the view: the session had no name.
BRANCH=$("$PYTHON" - "$TEST_DIR/pty.out" <<'PYBRANCH'
import re, sys
m = re.search(rb'on the branch (session/[A-Za-z0-9_.:-]+)', open(sys.argv[1], 'rb').read())
print(m.group(1).decode().rstrip('.') if m else '')
PYBRANCH
)
[ -n "$BRANCH" ] || fail 'the notice does not name the branch of the view'
run_fyai -b "$BRANCH" view diff session --stat
assert_status 0
assert_stdout_contains 'file'
assert_stdout_contains 'added'

# The bottom row of the prompt names the view that the session runs in.
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="/stream" FYAI_PTY_NEEDLE="/stream" \
FYAI_PTY_TIMEOUT=60 FYAI_PTY_AFTER="wait-screen:· view session|send:/exit" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty-view.out" \
    "$FYAI_BIN" -k test-key --set view/isolate_session=true \
    --theme dark --set display/markdown=true \
    --set display/renderer=page --set display/screen=fullscreen \
    --set display/stream=false --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
	fail "the bottom row does not name the view"

mock_stop 2
pass
