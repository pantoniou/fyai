#!/bin/bash
# SPDX-License-Identifier: MIT
# An option of a question that allows one choice can carry a preview in
# Markdown. The page renders the preview of the option under the cursor below
# the options, and it follows the cursor. A fenced mermaid block is drawn as a
# diagram. An option with no preview shows none.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ask_user_preview.json
driver=0
FYAI_PTY_COLS=100 FYAI_PTY_INPUT="ask me about the layout" \
FYAI_PTY_NEEDLE="Which layout?" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:Preview · flat|wait-screen:main.c|raw:1b5b42|wait-screen:Preview · tree|wait-gone:main.c|wait-screen:│ Parser │|wait-screen:│ Emitter │|raw:1b5b42|wait-gone:Preview · tree|wait-gone:Emitter|raw:1b5b41|wait-screen:Preview · tree|raw:0d|wait-screen:Layout chosen." \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    driver=$?
if grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out"; then
    skip "this build has no page support"
fi
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the previews did not follow the cursor"
fi
assert_request 1 'json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])["answers"][0]["selected"] == ["tree"]'
mock_stop 2

pass
