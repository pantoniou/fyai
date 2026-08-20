#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify interrupted shell-result storage under Valgrind.
set -eu
. "$(dirname "$0")/../harness.sh"

command -v valgrind >/dev/null 2>&1 || {
	echo "SKIP: $(basename "$0") (valgrind not installed)"
	exit 0
}
# Skip Valgrind installations that cannot run the memcheck tool.
valgrind --tool=memcheck true >/dev/null 2>&1 || {
	echo "SKIP: $(basename "$0") (valgrind cannot run on this host)"
	exit 0
}
# An ASAN-instrumented binary cannot run under valgrind: its runtime refuses to
# start unless it is first in the library list.
[ "${FYAI_ASAN:-0}" = "1" ] && {
	echo "SKIP: $(basename "$0") (ASAN build)"
	exit 0
}

fyai_test_setup
mock_start native_shell_interrupt.json

VG_DIR="$TEST_DIR/vg"
mkdir -p "$VG_DIR"

# Memcheck can make initialization substantially slower on shared CI runners.
# Keep this below the case's 60-second CTest limit while avoiding the PTY
# driver's normal 15-second interactive-session deadline.
FYAI_PTY_INPUT="run it" \
FYAI_PTY_TIMEOUT="45" \
FYAI_PTY_PROGRESS_NEEDLE="nsiRAN" \
FYAI_PTY_PROGRESS_TIMEOUT="20" \
FYAI_PTY_INTERRUPT_AFTER_PROGRESS="1" \
FYAI_PTY_NEEDLE="interrupted" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$(command -v valgrind)" --log-file="$VG_DIR/vg.%p.txt" \
    --child-silent-after-fork=no --error-exitcode=0 \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set builtin_shell=true --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

mock_stop 1

for log in "$VG_DIR"/vg.*.txt; do
	[ -e "$log" ] || fail "valgrind produced no log"
	grep -qE "uninitialised|Invalid read|Invalid write|Invalid free" "$log" &&
		fail "memory error under valgrind: $(grep -m1 -A3 -E \
			'uninitialised|Invalid read|Invalid write|Invalid free' \
			"$log")"
	grep -q "default action of signal" "$log" &&
		fail "a process died from a default signal action: $(grep -m1 \
			'default action of signal' "$log")"
done

pass
