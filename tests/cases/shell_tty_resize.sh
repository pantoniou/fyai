#!/bin/bash
# SPDX-License-Identifier: MIT
# A pseudo-terminal follows the window of the user. The tool session leads its
# own session and cannot be told by the kernel, thus the size must reach it
# from the process that watches SIGWINCH.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

FYAI_PTY_ROWS=24 FYAI_PTY_COLS=80 \
FYAI_PTY_ROWS2=30 FYAI_PTY_COLS2=100 \
FYAI_PTY_DELAY=1.5 \
"$PYTHON" "$TESTS_DIR/pty_resize.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" tool shell \
    '{"command":"trap \"stty size\" WINCH; sleep 5 & p=$!; wait $p","tty":true,"timeout":10000}' \
    || fail "the resized tty run did not finish"

if ! grep -q "30 100" "$TEST_DIR/pty.out"; then
	echo "--- capture ---" >&2
	cat "$TEST_DIR/pty.out" >&2
	fail "the program was never told the new terminal size"
fi

pass
