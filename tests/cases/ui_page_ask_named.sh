#!/bin/bash
# A question given a name does not hold the turn. The call returns at once, list
# shows the question while the user has not answered, wait holds a call for the
# answers, and cancel takes the question back.
set -eu
. "$(dirname "$0")/../harness.sh"

# Run fyai under a terminal with the scenario $1, the keys $2 and the final
# text $3 on the screen.
ask()
{
    fyai_test_setup
    mock_start "$1"
    driver=0
    FYAI_PTY_COLS=100 FYAI_PTY_INPUT="ask me without waiting" \
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
        fail "the named question was not handled: $1"
    fi
}

TOOL='{m["tool_call_id"]: m["content"] for m in r["body"]["messages"] if m.get("role") == "tool"}'

# The wait of the same response holds until the user chooses; the list in
# between saw the question waiting.
ask ask_user_named.json "wait-screen:How wide?|wait-screen:still running|raw:32|wait-screen:Scope settled."
assert_request 1 "$TOOL[\"call_ask\"].startswith(\"[question 'ask-1' asked]\")"
assert_request 1 "json.loads($TOOL[\"call_list\"])[\"questions\"][0][\"name\"] == \"ask-1\""
# A poll of the same name looked once and found it running.
assert_request 2 "$TOOL[\"call_poll\"] == \"[still running: 'ask-1']\""
assert_request 3 "\"[question 'ask-1' answered]\" in $TOOL[\"call_wait\"] and \"medium\" in $TOOL[\"call_wait\"]"
mock_stop 4

# Cancel takes the question out of the input area, and list no longer has it.
ask ask_user_cancel.json "wait-gone:or type an answer|wait-screen:Question withdrawn."
assert_request 1 "$TOOL[\"call_cancel\"] == \"[question 'later' cancelled]\""
assert_request 1 "json.loads($TOOL[\"call_list\"])[\"questions\"] == []"
mock_stop 2

pass
