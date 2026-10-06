#!/bin/bash
# SPDX-License-Identifier: MIT
# The default bottom row of the prompt shows the isolation in effect, and a run
# without isolation shows nothing more than it did.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

fyai_test_setup
mock_start transport_reload.json

run_pty() {
	# $1 the reply to wait for, $2 what to wait for after it; the rest is the
	# environment of the run.
	local reply="$1" after="$2"
	shift 2
	env "$@" FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="question" FYAI_PTY_NEEDLE="question" \
	FYAI_PTY_AFTER="wait-screen:$reply|wait-screen:$after|send:/exit" \
	"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	    "$FYAI_BIN" -k test-key --theme dark -b main --set api=chat-completions \
	    --set "api_url=$MOCK_URL/v1/chat/completions" \
	    --set display/stream=false --set display/markdown=true \
	    --set display/screen=fullscreen \
	    -m mock-model -i
}

# With isolation the row ends with the level that is in effect.
run_pty "Reply one." "isolated level-b" FYAI_TRANSPORT_ISOLATION=level-b

# With `auto` it is the level that auto chose, not the word auto.
run_pty "Reply two." "isolated level-b" FYAI_TRANSPORT_ISOLATION=auto

# Without isolation the word never appears on the screen.
run_pty "Reply three." "mock-model" FYAI_TRANSPORT_ISOLATION=none
! grep -aq "isolated" "$TEST_DIR/pty.out" || fail "an unisolated run says it is isolated"
mock_stop 3
pass
