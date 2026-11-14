#!/bin/bash
# SPDX-License-Identifier: MIT
# A forked tool child serves the terminal session of a bang shell. When the
# process that owns the session is killed, the child loses its control channel:
# it must end the program it serves, not keep it running with nothing to show
# it to.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# The shell records its process and writes HOLD in two parts, so only its
# output holds the word and not the command in the head.
pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark -m mock-model -i <<'PTY' ||
rows: 30
cols: 100
timeout: 20
exit: {signal: 9}
steps:
  - type: "!sh -c 'echo $$$ > ${TEST_DIR}/shell.pid; printf %s%s HO LD; sleep 60'"
  - wait: bang-1
    timeout: session
  - wait-screen: HOLD
  - signal: 9
PTY
    fail "the session did not stand before its owner was killed"

[ -s "$TEST_DIR/shell.pid" ] || fail "the shell did not record its process"
shell=$(cat "$TEST_DIR/shell.pid")
"$PYTHON" -I "$TESTS_DIR/wait_exit.py" "$shell" $((100 * FYAI_TIMEOUT_SCALE)) || {
    kill -KILL "$shell" 2>/dev/null || true
    fail "the shell outlived the process that owned its session"
}

pass
