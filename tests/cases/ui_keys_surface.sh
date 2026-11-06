#!/bin/bash
# SPDX-License-Identifier: MIT
# The keys that leave a tile are keys of the surface mode: with display/keys
# Ctrl-] is unbound, so a program in the tile receives it, and F9 gives the
# keys back to the prompt.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# The terminal of the tile echoes the code of Ctrl-] as ^], which shows that
# the program got it. F9 is "ESC [ 2 0 ~".
pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key -m mock-model -i \
    --set 'display/keys={surface: {"Ctrl-]": "", F9: fyai.focus.prompt}}' <<'PTY' ||
timeout: 30
steps:
  - type: "!cat"
  - wait-screen: "Ctrl-] returns to the prompt"
  - write: "\x1d"
  - wait-screen: "^]"
  - wait-screen: "Ctrl-] returns to the prompt"
  - write: "\e[20~"
  - wait-gone: "Ctrl-] returns to the prompt"
PTY
    fail "the keys of the surface mode did not move"
pass
