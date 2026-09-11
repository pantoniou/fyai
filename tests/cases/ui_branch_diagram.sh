#!/bin/bash
# SPDX-License-Identifier: MIT
# Tree view retains selection and applies live display settings.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create main/child
assert_status 0
run_fyai branch create main/child/deep
assert_status 0
run_fyai branch create main-sibling
assert_status 0

FYAI_PTY_INPUT="/branches" \
FYAI_PTY_NEEDLE="╰─" \
FYAI_PTY_AFTER="raw:1b5b48|raw:6a6a|raw:1b5b44|wait-gone:- deep|raw:6a0d|wait:switched to branch main-sibling|send:/config set display/diagram_charset ascii|wait:display/diagram_charset|send:/branches|wait-frame:+-|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/diagram.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/diagram_charset=unicode \
    -m mock-model -i

grep -qF "switched to branch main-sibling" "$TEST_DIR/diagram.out" || \
	fail "tree navigation skipped the row after a collapsed group"

pass
