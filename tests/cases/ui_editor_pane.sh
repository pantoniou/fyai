#!/bin/bash
# SPDX-License-Identifier: MIT
# An editor for the prompt runs in a tile of the work pane by default: the tile
# of the editor stands while it runs, and what it wrote is the prompt when it
# ends. display/editor=terminal gives the editor the whole terminal, as before,
# and opens no tile.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

cat >"$TEST_DIR/editor.sh" <<'EOF'
#!/bin/sh
printf 'EDITOR-RUNNING\n'
sleep 2
printf 'edited prompt' >"$1"
EOF
chmod +x "$TEST_DIR/editor.sh"

session()
{
    local name="$1"; shift
    mock_start chat_stream.json
    EDITOR="$TEST_DIR/editor.sh" \
    FYAI_PTY_INPUT="draft prompt" \
    FYAI_PTY_EDIT_INPUT=1 \
    FYAI_PTY_PROGRESS_TIMEOUT=6 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/$name.out" \
        "$FYAI_BIN" -b "editor-$name" -k test-key --theme dark \
        --set display/markdown=true --set display/stream=true \
        --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i "$@"
    assert_request 0 \
        'r["body"]["messages"][-1]["content"] == "edited prompt"'
    mock_stop 1
}

session pane
grep -a -q "edit-1" "$TEST_DIR/pane.out" ||
    fail "the editor did not run in a tile of the work pane"
grep -a -q "EDITOR-RUNNING" "$TEST_DIR/pane.out" ||
    fail "the tile did not show the editor"

session terminal --set display/editor=terminal
grep -a -q "edit-1" "$TEST_DIR/terminal.out" &&
    fail "display/editor=terminal opened a tile for the editor" || true
grep -a -q "EDITOR-RUNNING" "$TEST_DIR/terminal.out" ||
    fail "the editor did not run on the terminal"

pass
