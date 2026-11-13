#!/bin/bash
# SPDX-License-Identifier: MIT
# display/focus_bg, with display/focus_mark wash, gives the tile that holds
# the keys a ground of its own.
# That the cells are drawn with it is the library's own claim, proved there
# against a terminal; what this case proves is that fyai asks for it - the
# colour reaches the terminal when a tile is focused, and only then.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
fyai_test_on_exit 'rm -rf "$CAPTURES"'

# A bang shell takes the keys as it opens, so the ground is asked for
# without any key having to be pressed.
run_with()
{
    name=$1
    shift
    fyai_test_setup
    pty_run "$TEST_DIR/$name.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true "$@" -m mock-model -i <<'PTY'
rows: 30
cols: 100
timeout: 25
steps:
  - type: "!sh -c 'printf GROUND; sleep 3'"
  - wait: GROUND
    timeout: session
  - wait-screen: "Ctrl-] returns to the prompt"
  - write: "\x1d"
  - wait-gone: "Ctrl-] returns to the prompt"
PTY
    cp "$TEST_DIR/$name.out" "$CAPTURES/$name.out"
}

run_with plain
run_with ground --set display/focus_mark=wash \
    --set 'display/focus_bg="#1c2b3a"'

"$PYTHON" - "$CAPTURES/plain.out" "$CAPTURES/ground.out" <<'PY' || \
    fail "focus_bg did not reach the terminal"
import sys

# The 24-bit background the configuration asked for.
want = b"\x1b[48;2;28;43;58m"
plain = open(sys.argv[1], "rb").read()
ground = open(sys.argv[2], "rb").read()
if want not in ground:
    raise SystemExit("a focused tile was drawn without its ground")
if want in plain:
    raise SystemExit("the ground was drawn with no colour configured")
PY

pass
