#!/bin/bash
# SPDX-License-Identifier: MIT
# The tty shell path returns the rendered terminal screen, not escape bytes.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai tool shell '{"command":"printf '\''plain\\n'\''; printf '\''\\033[2J\\033[Hscreen\\n'\''","tty":true}'
assert_status 0
assert_stdout_contains "screen"
if grep -q $'\033' "$TEST_DIR/stdout"; then
	fail "PTY output still contains terminal escape sequences"
fi

run_fyai tool shell '{"command":"exit 7","tty":true}'
assert_status 0
assert_stdout_contains "command exited with status 7"

# A descendant can inherit the terminal after the direct shell exits. The
# command completes with that shell and must not wait for the inherited slave
# descriptor to reach EOF.
if ! run_limited 5 "$FYAI_BIN" -k test-key --color off tool shell \
	'{"command":"sleep 30 & echo direct-done; exit 0","tty":true}' \
	>"$TEST_DIR/descendant.out" 2>&1 </dev/null; then
	cat "$TEST_DIR/descendant.out" >&2
	fail "a descendant that kept the PTY open kept the shell call alive"
fi
grep -q "direct-done" "$TEST_DIR/descendant.out" ||
	fail "the PTY drain lost output from the direct shell"

# A screen holds 24 rows. Everything above them must still be reported, thus
# the lines that scrolled off are kept in front of the visible screen.
run_fyai tool shell '{"command":"i=1; while [ $i -le 100 ]; do echo line-$i; i=$((i+1)); done","tty":true}'
assert_status 0
assert_stdout_contains "line-1"
assert_stdout_contains "line-50"
assert_stdout_contains "line-100"

# The model can ask for a size, and the program must be given it.
run_fyai tool shell '{"command":"tput cols; tput lines","tty":true,"rows":40,"cols":132}'
assert_status 0
assert_stdout_contains "132"
assert_stdout_contains "40"

# A full-screen program leaves its last screen behind: the alternate screen is
# not enabled, thus exiting it does not restore an empty one.
run_fyai tool shell '{"command":"tput smcup; clear; echo FULLSCREEN; tput rmcup","tty":true}'
assert_status 0
assert_stdout_contains "FULLSCREEN"

# The limit stops the command and keeps what it already printed.
run_fyai tool shell '{"command":"echo before; sleep 30","tty":true,"timeout":1500}'
assert_status 0
assert_stdout_contains "before"
assert_stdout_contains "command timed out"

# The program must be told which terminal it has, and how big it is. An
# inherited TERM names a terminal libfyvterm does not emulate.
run_fyai tool shell '{"command":"echo T=$TERM C=$COLUMNS L=$LINES","tty":true,"rows":40,"cols":132}'
assert_status 0
assert_stdout_contains "T=xterm-256color C=132 L=40"

# A screen is text by construction, thus a blob must be recognised on the raw
# stream and reported, not interpreted into a mangled screen.
run_fyai tool shell '{"command":"head -c 2000 /dev/urandom","tty":true}'
assert_status 0
assert_stdout_contains "binary output:"

# One screen row is not one line. A line longer than the screen is stored as
# several rows, and they must be joined again or the result cannot be searched.
run_fyai tool shell '{"command":"printf %200s | tr \" \" x; echo; echo tail","tty":true,"cols":80}'
assert_status 0
[ "$(awk 'NR==1 {print length($0)}' "$TEST_DIR/stdout")" = "200" ] || \
	fail "a line longer than the screen was not joined again"

# Text is UTF-8: a wide character occupies one cell and leaves the next unused,
# and a combining character belongs to the cell of its base character. The
# screen must carry the same bytes the pipe path returns.
utf8_cmd='{"command":"echo \u65e5\u672c\u8a9e e\u0301 end"'
run_fyai tool shell "$utf8_cmd,\"tty\":true}"
assert_status 0
cp "$TEST_DIR/stdout" "$TEST_DIR/tty.txt"
run_fyai tool shell "$utf8_cmd}"
assert_status 0
# The screen has no trailing blank rows, thus the text is compared and not the
# trailing newline the byte stream keeps.
[ "$(head -1 "$TEST_DIR/tty.txt")" = "$(head -1 "$TEST_DIR/stdout")" ] || \
	fail "the rendered screen does not match the byte stream for UTF-8 text"

# An interrupt must reach the command. Without it the only way out is the
# time limit. The run is backgrounded here so the case can signal it.
"$FYAI_BIN" -k test-key --color off tool shell \
	'{"command":"echo started; sleep 30","tty":true}' \
	>"$TEST_DIR/interrupt.out" 2>&1 </dev/null &
tty_pid=$!
# The result is the screen, which arrives when the command ends, thus there is
# nothing to wait for. Give the command time to start, then signal it.
sleep 2
kill -INT "$tty_pid"
wait "$tty_pid" || true
grep -q "started" "$TEST_DIR/interrupt.out" || \
	fail "the interrupted run lost the output it already had"
grep -q "interrupted" "$TEST_DIR/interrupt.out" || \
	fail "an interrupt did not stop the tty command"

pass
