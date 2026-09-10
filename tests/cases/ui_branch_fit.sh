#!/bin/bash
# SPDX-License-Identifier: MIT
# The gitgraph overview fits the pane it is given and states what it left out.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
for n in alpha-service bravo-service charlie-service delta-service echo-service foxtrot-service; do
	run_fyai branch create "main/$n"
	assert_status 0
done

# A short pane holds a part of the list. The page says how much it left out,
# and the branch it drew last is on the screen: a drawing that overran the
# pane would have been cut there.
FYAI_PTY_ROWS=20 FYAI_PTY_COLS=60 \
FYAI_PTY_INPUT="/branches" \
FYAI_PTY_NEEDLE="gitgraph overview" \
FYAI_PTY_AFTER="wait: more|wait:main/bravo-service|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/fit.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/branch_view=gitgraph \
    --set display/diagram_charset=unicode -m mock-model -i

pass
