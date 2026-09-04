#!/bin/bash
# SPDX-License-Identifier: MIT
# A key this program does not keep for itself belongs to the program that
# holds the keys. ^C is the plainest of them, and the one the terminal of
# this process takes first: it arrives as a signal, and has to be given to
# the program the user was typing into rather than to the turn.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# ^C at a focused shell: the terminal of the shell makes a signal of it, so
# the loop below is stopped by it. fyai must still be there afterwards, and
# answer for itself - with no ground of its own it would have taken the
# interrupt and left.
# The focus hint is drawn before input routing settles: yield one drain
# so the interrupt reaches the tile instead of the prompt behind it.
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!sh -c 'echo READY; while :; do sleep .2; done'" \
FYAI_PTY_NEEDLE="READY" FYAI_PTY_TIMEOUT=25 FYAI_PTY_AFTER_TIMEOUT=15 \
FYAI_PTY_AFTER="wait:Ctrl-]|settle:.5|raw:03|wait:signal 2|raw:1d|send:/sessions|"\
"wait:sessions" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'PYEOF' || fail "^C did not reach the session"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
# The shell's own terminal turned the byte into a signal for the shell.
if b"killed by signal 2" not in plain:
    raise SystemExit("the program never saw the interrupt")
# And this program was still there to say so, and to answer afterwards.
if b"sessions" not in plain:
    raise SystemExit("fyai took the interrupt for itself")
PYEOF

pass
