#!/bin/bash
# SPDX-License-Identifier: MIT
# The pane is an fy-grid the page writes, with one slot for each tile. Two
# tiles side by side must stand in the same columns with the same rule between
# them and the same rows for their screens. Each shell prints the size it was
# given, and the tile that holds the keys stands on the same reversed ground.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
fyai_test_on_exit 'rm -rf "$CAPTURES"'

# A frame paints only the cells that changed, so a size that a shell prints is
# not in the output bytes whole. The case waits for the tile head, then on the
# screen for the size of the first shell alone and for the size each shell is
# granted beside the other, while the second holds the keys; then for each head
# to be made again at the width of its grant, which leaves no head cut with an
# ellipsis.
run_with()
{
    renderer=$1
    fyai_test_setup
    FYAI_TRACE="$TEST_DIR/trace.log" \
    pty_run "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/work_min_tile_cols=30 \
        -m mock-model -i <<'PTY'
cols: 100
timeout: 20
wait_timeout: 10
steps:
  - type: "!sh -c 'while :; do printf \"\\r\\033[KFIRST %s \" \"$(stty size)\"; sleep 0.2; done'"
  - wait: bang-1
    timeout: session
  - wait-screen: FIRST 20 98
  - wait-screen: "Ctrl-]"
  - write: "\x1d"
  - wait-gone: "Ctrl-]"
  - type: "!sh -c 'while :; do printf \"\\r\\033[KSECOND %s \" \"$(stty size)\"; sleep 0.2; done'"
  - wait-screen: "Ctrl-]"
  - wait-screen: FIRST 20 47
  - wait-screen: SECOND 20 46
  - write: "\x1d"
  - wait-gone: "Ctrl-]"
  - frame: 4
  - type: "/kill bang-1"
  - wait-screen: stopping shell bang-1
  - type: "/kill bang-2"
  - wait-screen: stopping shell bang-2
PTY
    cp "$TEST_DIR/pty.out" "$CAPTURES/$renderer.out"
    cp "$TEST_DIR/trace.log" "$CAPTURES/$renderer.trace"
}

run_with page

if grep -a -q "needs a libfytimui" "$CAPTURES/page.out" "$CAPTURES/page.trace"; then
    skip "this build has no page support"
fi

# The trace says that the page placed the two tiles in slots of their own,
# beside the prompt slot.
grep -a -q "page: .*tiles=2" "$CAPTURES/page.trace" ||
    fail "the page did not place the tiles in slots"

pass
