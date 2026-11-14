#!/bin/bash
# SPDX-License-Identifier: MIT
# The case reads the transcript from the scrollback, thus it names the band
# stack: a fullscreen page repaints the cells that change and the bytes do not
# say what the screen shows. A named branch does not take the display of the
# arena, thus the case states it.
# An editor for the prompt runs in a tile of the work pane by default: the tile
# of the editor stands while it runs, and what it wrote is the prompt when it
# ends. display/editor=terminal gives the editor the whole terminal, as before,
# and opens no tile.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

session()
{
    local name="$1"; shift

    mock_start chat_stream.json
    # The case releases the editor when its screen shows that it runs: the
    # editor reads a FIFO, and the touch step of the driver opens it for
    # writing, which wakes the read. The FIFO is outside the project. The path
    # is in the script: the editor does not get the environment of the case.
    export MOCK_NOTIFY_DIR
    mkfifo "$MOCK_NOTIFY_DIR/release-$name" || fail "cannot make the FIFO"
    cat >"$TEST_DIR/editor-$name.sh" <<EOF
#!/bin/sh
printf 'EDITOR-RUNNING\n'
read -r _ <"$MOCK_NOTIFY_DIR/release-$name"
printf 'edited prompt' >"\$1"
EOF
    chmod +x "$TEST_DIR/editor-$name.sh"
    export name
    EDITOR="$TEST_DIR/editor-$name.sh" \
    pty_run "$TEST_DIR/$name.out" \
        "$FYAI_BIN" -b "editor-$name" -k test-key --theme dark \
        --set display/markdown=true --set display/stream=true \
        --set display/screen=inline \
        --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i "$@" <<'PTY'
steps:
  - write: "draft prompt\a"
  - wait: EDITOR-RUNNING
    timeout: 6
  - touch: "${MOCK_NOTIFY_DIR}/release-${name}"
  - wait: edited prompt
    timeout: 6
  - write: "\n"
  - wait: Streaming hello from the mock.
    timeout: session
PTY
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
