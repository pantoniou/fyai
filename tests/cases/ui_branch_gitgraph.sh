#!/bin/bash
# SPDX-License-Identifier: MIT
# The gitgraph overview cycles through list and tree without closing the browser.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create main/feature
assert_status 0
run_fyai branch create main/feature/worker
assert_status 0

FYAI_PTY_INPUT="/branches" \
FYAI_PTY_NEEDLE="gitgraph overview" \
FYAI_PTY_AFTER="raw:67|wait-frame:list · / filter|raw:67|wait-frame:╰─|raw:67|wait-frame:gitgraph overview|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/gitgraph.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/branch_view=gitgraph \
    --set display/diagram_charset=unicode -m mock-model -i

FYAI_PTY_INPUT="/branches" \
FYAI_PTY_NEEDLE="Diagram unavailable; showing list." \
FYAI_PTY_AFTER="raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/fallback.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/branch_view=gitgraph --set display/diagram_theme=missing-theme \
    -m mock-model -i

pass
