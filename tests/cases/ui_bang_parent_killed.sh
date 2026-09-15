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
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!sh -c 'echo \$\$ > $TEST_DIR/shell.pid; printf %s%s HO LD; sleep 60'" \
FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:HOLD|signal:9" \
FYAI_PTY_EXIT_SIGNAL=9 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark -m mock-model -i ||
    fail "the session did not stand before its owner was killed"

[ -s "$TEST_DIR/shell.pid" ] || fail "the shell did not record its process"
shell=$(cat "$TEST_DIR/shell.pid")
tries=$((100 * FYAI_TIMEOUT_SCALE))
while kill -0 "$shell" 2>/dev/null; do
    tries=$((tries - 1))
    if [ "$tries" -le 0 ]; then
        kill -KILL "$shell" 2>/dev/null || true
        fail "the shell outlived the process that owned its session"
    fi
    sleep 0.05
done

pass
