#!/bin/bash
# SPDX-License-Identifier: MIT
# Several questions in the input area of the page: one is shown at a time with
# its position, a question that allows many choices toggles with the number
# keys and Space, Left goes back, and a review of every answer ends the series
# before it is sent.
set -eu
. "$(dirname "$0")/../harness.sh"

# Ask the questions of the scenario and drive the keys $1; the result that the
# model was given is checked by $2, a Python expression over res.
ask()
{
    name=$1
    fyai_test_setup
    mock_start ask_user_multi.json
    driver=0
    FYAI_PTY_COLS=100 FYAI_PTY_INPUT="ask me two things" \
    FYAI_PTY_NEEDLE="How wide?" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="$2" \
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
        fail "the questions were not answered by $name"
    fi
    assert_request 1 "$3"
}

RES='json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])'

# The first question names its position, its options and their descriptions.
# The key 2 chooses and goes on; the second question toggles with the number
# keys; Enter ends the series at the review, and Enter again sends it.
ask review \
    "wait-screen:(1 of 2)|wait-screen:The medium choice|raw:32|wait-screen:Which checks?|wait-screen:(2 of 2)|wait-screen:Space toggles|raw:31|wait-screen:1. [x] lint|raw:33|wait-screen:3. [x] asan|raw:0d|wait-screen:Send these answers?|wait-screen:Scope: medium|wait-screen:Checks: lint, asan|raw:0d|wait-screen:Answers received." \
    "$RES[\"answers\"][0][\"selected\"] == [\"medium\"] and $RES[\"answers\"][1][\"selected\"] == [\"lint\", \"asan\"]"
mock_stop 2

# Left goes back to the first question, which keeps its answer: choosing
# another changes it, and the review shows the change.
ask back \
    "wait-screen:(1 of 2)|raw:32|wait-screen:(2 of 2)|raw:1b5b44|wait-screen:(1 of 2)|raw:33|wait-screen:(2 of 2)|raw:20|wait-screen:1. [x] lint|raw:0d|wait-screen:Scope: large|raw:0d|wait-screen:Answers received." \
    "$RES[\"answers\"][0][\"selected\"] == [\"large\"] and $RES[\"answers\"][1][\"selected\"] == [\"lint\"]"
mock_stop 2

# Text typed answers a question in place of its options, and Escape at the
# review declines the whole series.
ask text \
    "wait-screen:(1 of 2)|raw:6d|frame:2|raw:0d|wait-screen:(2 of 2)|raw:1b|wait-screen:Answers received." \
    "$RES[\"status\"] == \"declined\""
mock_stop 2

pass
