#!/bin/bash
# SPDX-License-Identifier: MIT
# The renderer blinks a quiet shell tile without Markdown or worker output.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
for mode in local split; do
    args=()
    if [ "$mode" = split ]; then
        args+=(--split)
    fi
    FYAI_TEST_NO_SPLIT=1 pty_run "$TEST_DIR/$mode.out" \
        "$FYAI_BIN" "${args[@]}" -k test-key --theme dark -m mock-model \
        --set display/markdown=false --set display/screen=fullscreen -i <<'PTY'
steps:
  - type: "!sh -c 'printf %s%s TILE READY; read x'"
  - wait-screen: TILEREADY
  - wait-screen: "● shell [bang-1]"
  - wait-gone: "● shell [bang-1]"
  - wait-screen: "● shell [bang-1]"
  - write: "\x1d"
PTY
done
pass
