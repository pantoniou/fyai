#!/bin/bash
# SPDX-License-Identifier: MIT
# $HOME can name the home directory through a symbolic link while getcwd()
# answers with the canonical path. The header abbreviates the working
# directory to ~ when the two name the same directory.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# $HOME reaches the same directory through a link, as a home on another
# filesystem does.
mkdir -p "$HOME/sub"
ln -s "$HOME" "$TEST_DIR/home-link"
export HOME="$TEST_DIR/home-link"

cd "$TEST_DIR/home/sub"
FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="" \
FYAI_PTY_NEEDLE="fyai:" FYAI_PTY_TIMEOUT=20 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark --set display/markdown=true \
    -m mock-model -i >/dev/null 2>&1 || true
cd "$TEST_DIR"

grep -a -q '~/sub' "$TEST_DIR/pty.out" || \
    fail "the header did not abbreviate a home reached through a link"
grep -a -q "fyai: .*$TEST_DIR/home/sub" "$TEST_DIR/pty.out" && \
    fail "the header named the canonical path"

pass
