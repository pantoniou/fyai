#!/bin/bash
# SPDX-License-Identifier: MIT
# /resume and /switch resume another session inside a running one: by name,
# or through the picker, where Escape returns to the session.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create newer
assert_status 0

# `newer` holds the mock provider settings and is the newest session, so
# the picker opens on it.
mock_start chat_basic.json
run_fyai -b newer --set api=chat-completions \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	--set display/stream=false
assert_status 0

# --- /resume NAME -----------------------------------------------------------
FYAI_PTY_INPUT="/resume newer" \
FYAI_PTY_NEEDLE="/resume newer" \
FYAI_PTY_AFTER="send:say hello|wait-screen:Hello from the mock provider." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/name.out" \
	"$FYAI_BIN" -k test-key -m mock-model -b main -i
mock_stop 1

run_fyai -b newer dump state
assert_status 0
[ "$(grep -c "Hello from the mock provider." "$TEST_DIR/stdout")" = 1 ] || \
	fail "/resume did not continue the named session"
assert_state_absent "Hello from the mock provider." -b main dump state

# --- /switch opens the picker; Escape returns, Enter resumes ----------------
# The needle matches bytes before the frame that holds them ends, so each key
# waits for the picker on the screen: a wait-gone on a picker not yet drawn
# passes at once, and the keys after it reach the old picker.
# The new mock provider listens on another port.
mock_start chat_basic.json
run_fyai -b newer --set "api_url=$MOCK_URL/v1/chat/completions"
assert_status 0
FYAI_PTY_INPUT="/switch" \
FYAI_PTY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_AFTER="wait-screen:toggle foreign sessions|raw:1b|wait-gone:toggle foreign sessions|send:/switch|wait-screen:fyai · newer|raw:0d|wait-gone:toggle foreign sessions|send:say hello|wait-screen:Hello from the mock provider." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/picker.out" \
	"$FYAI_BIN" -k test-key -m mock-model -b main -i
mock_stop 1

run_fyai -b newer dump state
assert_status 0
[ "$(grep -c "Hello from the mock provider." "$TEST_DIR/stdout")" = 2 ] || \
	fail "the picker did not resume the selected session"
assert_state_absent "Hello from the mock provider." -b main dump state

pass
