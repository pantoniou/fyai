#!/bin/bash
# SPDX-License-Identifier: MIT
# The prompt editor preserves Unicode branch names across deletion and submit.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create existing
assert_status 0

FYAI_PTY_INPUT="/branches" \
FYAI_PTY_PROGRESS_NEEDLE="Branches" \
FYAI_PTY_DURING_INPUT=$'nδοκιμή/日本語界\177\r\r' \
FYAI_PTY_DURING_SUBMIT=0 \
FYAI_PTY_NEEDLE="created branch δοκιμή/日本語" \
FYAI_PTY_AFTER="raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/browser.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true -m mock-model -i

run_fyai branch show δοκιμή/日本語
assert_status 0
assert_stdout_contains "δοκιμή/日本語"
pass
