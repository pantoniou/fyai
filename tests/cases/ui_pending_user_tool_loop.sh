#!/bin/bash
# SPDX-License-Identifier: MIT
# A queued user message joins the next request after a tool call completes.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
printf 'tool input\n' > data.txt
mock_start chat_tool_pending_user.json

FYAI_PTY_INPUT="read the file" \
FYAI_PTY_DURING_INPUT="also explain the result" \
FYAI_PTY_DURING_DELAY=0.2 \
FYAI_PTY_NEEDLE="I saw the follow-up during the tool loop." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pending.out" \
	"$FYAI_BIN" -b main -k test-key -m mock-model -i \
	--set display/stream=false --set api=chat-completions \
	--set tools=true --set "api_url=$MOCK_URL/v1/chat/completions"

mock_stop 2
assert_request 0 'r["body"]["messages"][-1]["content"] == "read the file"'
assert_request 1 'any(m.get("role") == "user" and m.get("content") == "also explain the result" for m in r["body"]["messages"])'
assert_request 1 'any(m.get("role") == "tool" for m in r["body"]["messages"])'
assert_state_contains "also explain the result" dump state

"$PYTHON" - "$SCENARIOS_DIR/responses_tools_read_file.json" \
	"$TEST_DIR/responses-pending.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as fp:
    scenario = json.load(fp)
scenario["steps"][0]["delay"] = 2.0
with open(sys.argv[2], "w", encoding="utf-8") as fp:
    json.dump(scenario, fp)
PY
mock_start "$TEST_DIR/responses-pending.json"

FYAI_PTY_INPUT="read the file" \
FYAI_PTY_DURING_INPUT="also explain the result" \
FYAI_PTY_DURING_DELAY=0.2 \
FYAI_PTY_NEEDLE="The file says: mock data payload." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/responses.out" \
	"$FYAI_BIN" -b main -k test-key -m mock-model -i \
	--set display/stream=false --set api=responses \
	--set response_chain=true --set tools=true \
	--set "api_url=$MOCK_URL/v1/responses"

mock_stop 2
assert_request 1 'any(i.get("role") == "user" for i in r["body"]["input"])'
assert_request 1 'any(i.get("type") == "function_call_output" for i in r["body"]["input"])'
pass
