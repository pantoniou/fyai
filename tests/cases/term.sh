#!/bin/bash
# SPDX-License-Identifier: MIT
# `fyai term` draws a program itself: the bytes of the program never reach the
# terminal, thus what the user sees is the screen fyai painted from its view.
# Each check reads the capture back through the screen model.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# The screen as it stood when the second argument first appeared. A capture
# replayed whole ends blank, because leaving erases the band: the reading has
# to be taken at the moment the content was on screen.
screen_at() {
	FYAI_SCREEN_ROWS="${FYAI_TERM_ROWS:-24}" \
	FYAI_SCREEN_COLS="${FYAI_TERM_COLS:-80}" \
	"$PYTHON" - "$1" "$2" <<'SCREENPY'
import os, sys
sys.path.insert(0, os.environ["TESTS_DIR"])
from screen import rows_at
rows = int(os.environ.get("FYAI_SCREEN_ROWS", "24"))
cols = int(os.environ.get("FYAI_SCREEN_COLS", "80"))
for row in rows_at(sys.argv[1], sys.argv[2].encode(), rows=rows, cols=cols):
    print(row)
SCREENPY
}

# 1. What the program drew is painted, and fyai owns the last row.
FYAI_TERM_ROWS=24 FYAI_TERM_COLS=80 \
FYAI_TERM_WAIT='prompt>' FYAI_TERM_SEND='ping\n' FYAI_TERM_WAIT2='got:ping' \
"$PYTHON" "$TESTS_DIR/term_driver.py" "$TEST_DIR/term.out" \
    "$FYAI_BIN" term -c 'printf "prompt> "; read x; printf "\ngot:%s\n" "$x"' \
    || fail "the terminal run did not finish"

screen_at "$TEST_DIR/term.out" "got:ping" > "$TEST_DIR/term.screen"
grep -q "got:ping" "$TEST_DIR/term.screen" ||
	{ cat "$TEST_DIR/term.screen" >&2; fail "the typed answer was never painted"; }
grep -q "fyai term" "$TEST_DIR/term.screen" ||
	{ cat "$TEST_DIR/term.screen" >&2; fail "the status row was never painted"; }

# 2. The prefix key belongs to fyai: ^\ q leaves while the program still runs.
# The two keys go in one write, which is the case that needs them to arrive in
# the order they were typed: reversed, the program is sent the q and stays.
FYAI_TERM_ROWS=24 FYAI_TERM_COLS=80 \
FYAI_TERM_WAIT='alive' FYAI_TERM_SEND='\x1cq' \
FYAI_TERM_TIMEOUT=15 \
"$PYTHON" "$TESTS_DIR/term_driver.py" "$TEST_DIR/quit.out" \
    "$FYAI_BIN" term -c 'echo alive; sleep 30' \
    || fail "the prefix key did not leave the terminal"

# 2b. A program that ignores being asked to leave is made to. An interactive
# shell ignores SIGTERM, so this is what ^\ q meets in normal use: fyai must
# still end, and end by itself.
FYAI_TERM_ROWS=24 FYAI_TERM_COLS=80 \
FYAI_TERM_WAIT='alive' FYAI_TERM_SEND='\x1cq' FYAI_TERM_TIMEOUT=20 \
"$PYTHON" "$TESTS_DIR/term_driver.py" "$TEST_DIR/stubborn.out" \
    "$FYAI_BIN" term -c 'trap "" TERM HUP; echo alive; sleep 60' \
    || fail "a program that ignores a signal kept fyai alive"

# The asking is a hangup first, which is what the end of a terminal is: an
# interactive shell ignores SIGTERM and leaves on this one.
FYAI_TERM_ROWS=24 FYAI_TERM_COLS=80 \
FYAI_TERM_WAIT='alive' FYAI_TERM_SEND='\x1cq' FYAI_TERM_WAIT2='GOTHUP' \
FYAI_TERM_TIMEOUT=20 \
"$PYTHON" "$TESTS_DIR/term_driver.py" "$TEST_DIR/hup.out" \
    "$FYAI_BIN" term -c 'trap "echo GOTHUP; exit 0" HUP; echo alive; sleep 60' \
    || fail "the program was never sent a hangup"

# 2c. A descendant that keeps the terminal open does not keep fyai: the end is
# the program being reaped and never the stream reaching its end.
FYAI_TERM_ROWS=24 FYAI_TERM_COLS=80 \
FYAI_TERM_WAIT='alive' FYAI_TERM_TIMEOUT=20 \
"$PYTHON" "$TESTS_DIR/term_driver.py" "$TEST_DIR/linger.out" \
    "$FYAI_BIN" term -c 'echo alive; sleep 30 & exit 0' \
    || fail "a lingering descendant kept fyai alive"

# 3. The program is given the rows the band granted it, not the height of the
# window: the prompt, the separators and the state row take rows of their own.
# Then the window is resized, and both the rows and the width must reach it.
# The trap must survive being interrupted, so the wait is a loop of short
# sleeps and not one long one.
cat > "$TEST_DIR/winch.sh" <<'SH'
trap 'stty size | sed "s/^/size /"' WINCH
echo ready
i=0
while [ $i -lt 40 ]; do sleep 0.2; i=$((i+1)); done
SH

FYAI_TERM_ROWS=24 FYAI_TERM_COLS=80 \
FYAI_TERM_WAIT='ready' FYAI_TERM_RESIZE_DELAY=0.5 FYAI_TERM_RESIZE='40x100' \
FYAI_TERM_WAIT2='size ' FYAI_TERM_SEND='\x1c' FYAI_TERM_SEND2='q' \
FYAI_TERM_SEND_GAP=1.0 FYAI_TERM_TIMEOUT=40 \
"$PYTHON" "$TESTS_DIR/term_driver.py" "$TEST_DIR/resize.out" \
    "$FYAI_BIN" term -c "sh $TEST_DIR/winch.sh" \
    || fail "the resized terminal run did not finish"

# The program reports its size each time it is told, so the reading is the
# capture itself and not one screen out of it: the last report is the one that
# matters, and a screen taken at the first would show the size before.
# The width is the window's, and the rows are one fewer than its 40: the state
# row of the surface is the only row that is not the program's.
grep -aqE "size (3[0-9]) 100" "$TEST_DIR/resize.out" ||
	{ grep -ao "size [0-9]* [0-9]*" "$TEST_DIR/resize.out" >&2
	  fail "the program was never told the new size"; }

# The state row shows the size that fyai set on the terminal of the program,
# thus it is the record of each size. Every size must be a size of the window.
# A grant that uses the rows of the last frame gives the old height with the
# new width.
if grep -aoE "fyai term  [a-z0-9 ]+  [0-9]+x100" "$TEST_DIR/resize.out" |
		grep -qvE " 3[0-9]x100$"; then
	grep -ao "fyai term  [a-z0-9 ]*  [0-9]*x[0-9]*" "$TEST_DIR/resize.out" >&2
	fail "the program was given a height the window never had"
fi

pass
