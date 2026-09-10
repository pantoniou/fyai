#!/bin/bash
# SPDX-License-Identifier: MIT
# A browser opens in the work pane and creates a branch through the editor.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create existing
assert_status 0

after="raw:69|wait-frame:Field|raw:1b|wait-frame:Branches|"
after="${after}raw:6b0d|wait-gone:Branches|wait-frame:existing|raw:1d|drain:0.2"

FYAI_PTY_INPUT="/branches" \
FYAI_PTY_PROGRESS_NEEDLE="Branches" \
FYAI_PTY_DURING_INPUT=$'nreview\r\r' \
FYAI_PTY_DURING_SUBMIT=0 \
FYAI_PTY_NEEDLE="created branch review" \
FYAI_PTY_AFTER="$after" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/browser.out" \
	"$FYAI_BIN" -k test-key --theme catppuccin:dark \
	--set display/markdown=true -m mock-model -i

run_fyai branch list
assert_status 0
assert_stdout_contains "review"
pass
