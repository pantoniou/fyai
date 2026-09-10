#!/bin/bash
# SPDX-License-Identifier: MIT
# Tree and list views retain selection and apply live display settings.
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
FYAI_PTY_AFTER="raw:6a6767|wait-frame:main/child|raw:67|wait-frame:╰─|raw:1d|send:/config set display/diagram_charset ascii|wait:display/diagram_charset|send:/branches|wait-frame:+-|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/diagram.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/diagram_charset=unicode \
    -m mock-model -i

pass
