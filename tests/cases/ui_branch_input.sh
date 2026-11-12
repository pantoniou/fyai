#!/bin/bash
# SPDX-License-Identifier: MIT
# The branch browser takes one input at a time, and Escape removes one layer
# at a time: the action form first, then the details under it, then the
# browser. A filter narrows the rows.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
for n in b03 b07; do
	run_fyai branch create "main/$n"
	assert_status 0
done

pty_run "$TEST_DIR/input.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/diagram_charset=unicode \
    -m mock-model -i <<'PTY'
rows: 24
cols: 80
steps:
  - type: "/branches"
  - wait: Branches
    timeout: session
  - write: "a"
  - wait-screen: Branch actions
  - write: "n"
  - wait-screen: Escape cancels
  - wait-gone: Branch actions
  - write: "\e"
  - wait-gone: Escape cancels
  - wait-screen: Branch actions
  - write: "\e"
  - wait-gone: Branch actions
  - wait-screen: b03
  - wait-screen: b07
  - write: "/"
  - wait-screen: "/ filter:"
  - write: "b07\r"
  - wait-gone: b03
  - wait-screen: b07
  - write: "\e"
  - wait-gone: Esc close
PTY
