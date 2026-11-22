#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a shell command of many lines shows its first line and a count
# of the rest, that display/tool_command_lines=0 shows all of it, and that
# the model and the stored exchange keep the whole command.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" - "$TEST_DIR/command_lines.json" <<'PY'
import json
import sys

command = "echo first-row\necho second-row\necho third-row"
call = {"id": "call_lines", "type": "function",
        "function": {"name": "exec_command",
                     "arguments": json.dumps({"command": command,
                                              "description": "three rows"})}}


def reply(message, reason):
    return {"response": {"id": "chatcmpl-lines", "object": "chat.completion",
                         "model": "mock-model",
                         "choices": [{"index": 0, "finish_reason": reason,
                                      "message": message}]}}


steps = [reply({"role": "assistant", "content": None, "tool_calls": [call]},
               "tool_calls"),
         reply({"role": "assistant", "content": "ran it"}, "stop")]
json.dump({"steps": steps}, open(sys.argv[1], "w"), indent=1)
PY
mock_start "$TEST_DIR/command_lines.json"

run_fyai --set display/markdown=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "run three rows"
assert_status 0

"$FYAI_BIN" --color off history --last 1 > "$TEST_DIR/one.txt"
"$FYAI_BIN" --color off --set display/tool_command_lines=0 history --last 1 \
	> "$TEST_DIR/all.txt"

"$PYTHON" - "$TEST_DIR/one.txt" "$TEST_DIR/all.txt" \
	"$TEST_DIR/requests.jsonl" <<'PY' || fail "the command rows are wrong"
import json
import sys

one = open(sys.argv[1]).read()
everything = open(sys.argv[2]).read()
# The output of the command has "row" too: count the command rows alone.
if "echo first-row" not in one or "echo second-row" in one or \
        "echo third-row" in one:
    raise SystemExit("not one command row:\n" + one)
if "⋯ 2 more lines" not in one:
    raise SystemExit("no count of the rows left out:\n" + one)
for row in ("echo first-row", "echo second-row", "echo third-row"):
    if row not in everything:
        raise SystemExit("a row of the command is missing with 0:\n" +
                         everything)
# The model got the whole command back with its result.
requests = [json.loads(line) for line in open(sys.argv[3])]
sent = json.dumps(requests[-1]["body"]["messages"])
if "echo third-row" not in sent:
    raise SystemExit("the model did not keep the whole command")
PY

mock_stop 2
pass
